#!/usr/bin/env python3
"""TTH Bot - M5Stack Core2 USB-serial provisioning (PHASE6_PLAN Step 6.1).

Writes the Wi-Fi credentials, gateway URL, device id and device token into the
robot's NVS namespace "tth" over the USB serial port. Nothing is compiled into
the firmware, and nothing secret is printed - not by the robot, and not by
this tool.

Run it with the PlatformIO Python, which already has pyserial:

    & "$env:USERPROFILE\\.platformio\\penv\\Scripts\\python.exe" tools\\provision.py --port COM3

Values come from environment variables when set, otherwise from prompts
(passwords and tokens are read without echo):

    TTH_WIFI_SSID  TTH_WIFI_PASSWORD  TTH_GATEWAY_URL  TTH_DEVICE_ID  TTH_DEVICE_TOKEN

Leave the device token empty to generate a new one. The token itself is never
shown: the tool prints only the registry entry for the gateway, which holds
the token's SHA-256 digest.

Gateway CA (Step 6.2; a PUBLIC certificate, never a key):
    --ca PEM        store this CA after the configuration
    --ca-only PEM   store only the CA

Change one part of the stored configuration, keeping everything else (the
device token included - no new registry entry needed):
    --set-wifi      new SSID and password (prompted, hidden)
    --set-url URL   new gateway URL

Other modes:
    --show    print the robot's redacted configuration report
    --reset   erase the TTH Bot configuration, CA included (asks for confirmation)

Close the serial monitor first: only one program can hold the port.
"""

import argparse
import getpass
import hashlib
import json
import os
import re
import secrets
import sys
import time

try:
    import serial
    import serial.tools.list_ports
except ImportError:
    sys.exit(
        "pyserial is missing. Run this tool with the PlatformIO Python:\n"
        '  & "$env:USERPROFILE\\.platformio\\penv\\Scripts\\python.exe" tools\\provision.py ...'
    )

BAUD = 115200
SLICE_BYTES = 32  # small writes, so the robot's loop is never flooded
SLICE_GAP_S = 0.01

# --- validation (mirrors lib/tth_core/src/DeviceConfig.cpp) ---------------------

HOST_LABEL = re.compile(r"^[A-Za-z0-9](?:[A-Za-z0-9-]{0,61}[A-Za-z0-9])?$")
PATH_RE = re.compile(r"^/[A-Za-z0-9._~/-]*$")
DEVICE_ID_RE = re.compile(r"^[A-Za-z0-9_-]{1,32}$")
DEVICE_TOKEN_RE = re.compile(r"^[A-Za-z0-9_-]{43}$")


def check_ssid(value: bytes):
    if not value:
        return "empty"
    if len(value) > 32:
        return "too long (1-32 bytes)"
    if any(b < 0x20 or b == 0x7F for b in value):
        return "control characters are not allowed"
    return None


def check_password(value: bytes):
    if not value:
        return None  # open network (confirmed separately)
    if len(value) == 64:
        if not re.fullmatch(rb"[0-9A-Fa-f]{64}", value):
            return "64 characters but not hexadecimal"
        return None
    if len(value) > 64:
        return "too long"
    if len(value) < 8:
        return "too short (8-63 characters)"
    if any(b < 0x20 or b > 0x7E for b in value):
        return "only printable ASCII is allowed"
    return None


def check_url(url: str):
    if not url:
        return "empty"
    if len(url) > 200:
        return "too long (200 characters maximum)"
    if any(ord(c) <= 0x20 or ord(c) >= 0x7F for c in url):
        return "invalid character (whitespace, control or non-ASCII)"
    if url.startswith("ws://"):
        return "insecure scheme ws:// refused - only wss:// is allowed"
    if not url.startswith("wss://"):
        return "unsupported scheme - only wss:// is allowed"
    rest = url[6:]
    if "?" in rest or "#" in rest:
        return "query or fragment is not allowed"
    slash = rest.find("/")
    authority = rest if slash < 0 else rest[:slash]
    if "@" in authority:
        return "user info (@) is not allowed"
    if slash < 0:
        return "missing path (for example /v1/ws)"
    if not authority or authority.startswith("[") or authority.count(":") > 1:
        return "invalid host"
    host, colon, port = authority.partition(":")
    if colon:
        if not re.fullmatch(r"[0-9]{1,5}", port) or not 1 <= int(port) <= 65535:
            return "invalid port (1-65535)"
    if not host or len(host) > 253:
        return "invalid host"
    if re.fullmatch(r"[0-9.]+", host):
        parts = host.split(".")
        if len(parts) != 4 or any(
            not re.fullmatch(r"[0-9]{1,3}", p) or int(p) > 255 for p in parts
        ):
            return "invalid IPv4 address"
    elif not all(HOST_LABEL.match(label) for label in host.split(".")):
        return "invalid host name"
    path = rest[slash:]
    if len(path) > 180 or not PATH_RE.match(path):
        return "invalid path"
    return None


def fail(message: str):
    sys.exit("provision: " + message)


# --- the serial link ---------------------------------------------------------------


class Robot:
    def __init__(self, port: str, secrets_to_hide):
        self._hide = [s for s in secrets_to_hide if s]
        self._buffer = b""
        link = serial.Serial()
        link.port = port
        link.baudrate = BAUD
        link.timeout = 0.05
        # Opening the port must not reset the Core2.
        link.dtr = False
        link.rts = False
        try:
            link.open()
        except serial.SerialException as error:
            fail(f"cannot open {port}: {error}\n(close the serial monitor first)")
        self._link = link

    def close(self):
        self._link.close()

    def _redact(self, line: str) -> str:
        # Defence in depth: the robot never prints a secret, but if a line ever
        # contained one, it still would not reach this terminal.
        for secret in self._hide:
            for form in (secret, secret.hex(), secret.hex().upper()):
                text = form if isinstance(form, str) else form.decode("utf-8", "replace")
                if text and text in line:
                    line = line.replace(text, "<redacted>")
        return line

    def send(self, command: str):
        data = ("!" + command + "\n").encode("utf-8")
        for i in range(0, len(data), SLICE_BYTES):
            self._link.write(data[i : i + SLICE_BYTES])
            self._link.flush()
            time.sleep(SLICE_GAP_S)

    def read_line(self, deadline: float):
        while time.monotonic() < deadline:
            newline = self._buffer.find(b"\n")
            if newline >= 0:
                raw, self._buffer = self._buffer[:newline], self._buffer[newline + 1 :]
                return self._redact(raw.decode("utf-8", "replace").rstrip("\r"))
            chunk = self._link.read(256)
            if chunk:
                self._buffer += chunk
        return None

    def command(self, command: str, timeout: float = 4.0) -> str:
        """Sends one command and returns its first [prov] reply line."""
        self.send(command)
        deadline = time.monotonic() + timeout
        while True:
            line = self.read_line(deadline)
            if line is None:
                fail("no reply from the robot (is it running the Step 6.1 firmware?)")
            if "[prov]" in line:
                return line

    def collect(self, seconds: float, markers=("[config]", "[net]", "[prov]")):
        deadline = time.monotonic() + seconds
        lines = []
        while True:
            line = self.read_line(deadline)
            if line is None:
                return lines
            if any(m in line for m in markers):
                lines.append(line)

    def wait_until_ready(self, seconds: float = 15.0):
        self._link.reset_input_buffer()
        self._link.write(b"\n")
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            self.send("prov ping")
            reply_deadline = min(deadline, time.monotonic() + 1.0)
            while True:
                line = self.read_line(reply_deadline)
                if line is None:
                    break
                if "[prov] pong" in line:
                    self._link.reset_input_buffer()
                    self._buffer = b""
                    return
        fail("the robot did not answer '!prov ping' (wrong port, old firmware, or still booting)")


def expect_ok(robot: Robot, command: str, label: str, abort: str = "prov abort"):
    reply = robot.command(command)
    if "ERROR" in reply:
        robot.command(abort)
        fail(f"{label} refused by the robot: {reply}\n{label}: nothing was stored")
    return reply


# --- the gateway CA (public) -------------------------------------------------------------

CA_LINE_RE = re.compile(r"^(?:[A-Za-z0-9+/=]+|-----(?:BEGIN|END) CERTIFICATE-----)$")


def load_ca(path: str):
    """Reads and checks a PEM CA bundle; returns its non-empty lines."""
    try:
        with open(path, "rb") as handle:
            data = handle.read()
    except OSError as error:
        fail(f"cannot read {path}: {error}")
    if len(data) > 4096:
        fail("the CA bundle is larger than 4096 bytes")
    try:
        text = data.decode("ascii")
    except UnicodeDecodeError:
        fail("the CA bundle is not ASCII PEM")
    if "PRIVATE KEY" in text:
        fail("that file contains a PRIVATE KEY - provision the CA certificate (ca.pem), never a key")
    lines = [line.strip() for line in text.splitlines() if line.strip()]
    if not lines or lines[0] != "-----BEGIN CERTIFICATE-----" or lines[-1] != "-----END CERTIFICATE-----":
        fail("not a PEM certificate bundle")
    for line in lines:
        if len(line) > 100 or not CA_LINE_RE.match(line):
            fail("unexpected line in the CA bundle")
    return lines


def send_ca(robot: Robot, lines):
    expect_ok(robot, "prov ca begin", "CA", abort="prov ca abort")
    for line in lines:
        expect_ok(robot, "prov ca line " + line, "CA", abort="prov ca abort")
    reply = robot.command("prov ca commit", timeout=6.0)
    if "CA COMMITTED" not in reply:
        robot.command("prov ca abort")
        fail(f"CA commit failed: {reply}\nthe previous CA is unchanged")
    print(reply)


# --- modes ---------------------------------------------------------------------------


def prompt_text(env: str, question: str) -> str:
    value = os.environ.get(env)
    if value is not None:
        print(f"{question}: (from {env})")
        return value
    return input(f"{question}: ")


def prompt_secret(env: str, question: str, confirm: bool) -> str:
    value = os.environ.get(env)
    if value is not None:
        print(f"{question}: (from {env})")
        return value
    first = getpass.getpass(f"{question}: ")
    if confirm and first:
        if getpass.getpass(f"{question} (again): ") != first:
            fail("the two entries did not match; nothing was sent")
    return first


def prompt_wifi():
    ssid = prompt_text("TTH_WIFI_SSID", "Wi-Fi network name (SSID)").encode("utf-8")
    problem = check_ssid(ssid)
    if problem:
        fail(f"SSID: {problem}")

    password = prompt_secret("TTH_WIFI_PASSWORD", "Wi-Fi password (empty = open network)", True)
    password_bytes = password.encode("utf-8")
    problem = check_password(password_bytes)
    if problem:
        fail(f"Wi-Fi password: {problem}")
    if not password_bytes:
        if input("No password: this is an OPEN network. Continue? [y/N] ").strip().lower() != "y":
            fail("cancelled; nothing was sent")
    return ssid, password_bytes


def update_stored(port: str, commands, secrets_to_hide):
    """Edits a copy of the STORED configuration and commits it atomically.

    Everything not in `commands` - the device token included - stays as it is.
    Any refusal aborts the edit and leaves the stored configuration unchanged.
    """
    robot = Robot(port, secrets_to_hide)
    try:
        robot.wait_until_ready()
        reply = robot.command("prov begin")
        if "from the stored configuration" not in reply:
            robot.command("prov abort")
            fail("nothing is stored on the robot yet - run a full provisioning first")
        print("robot answered; sending the change (values are not shown)")
        for command, label in commands:
            expect_ok(robot, command, label)
        reply = robot.command("prov commit", timeout=6.0)
        if "COMMITTED" not in reply:
            robot.command("prov abort")
            fail(f"commit failed: {reply}\nthe previous configuration is unchanged")
        print(reply)
        robot.send("prov show")
        for line in robot.collect(1.5):
            print(line)
    finally:
        robot.close()


def set_wifi(port: str):
    ssid, password_bytes = prompt_wifi()
    update_stored(
        port,
        [("prov sethex ssid " + ssid.hex(), "SSID"), ("prov sethex pass " + password_bytes.hex(), "Wi-Fi password")],
        [ssid, password_bytes],
    )


def set_url(port: str, url: str):
    url = url.strip()
    problem = check_url(url)
    if problem:
        fail(f"gateway URL: {problem}")
    update_stored(port, [("prov set url " + url, "gateway URL")], [])


def provision(port: str, ca_lines):
    ssid, password_bytes = prompt_wifi()

    url = prompt_text("TTH_GATEWAY_URL", "Gateway URL (wss://...)").strip()
    problem = check_url(url)
    if problem:
        fail(f"gateway URL: {problem}")

    device_id = prompt_text("TTH_DEVICE_ID", "Device id (1-32 of A-Z a-z 0-9 _ -)").strip()
    if not DEVICE_ID_RE.match(device_id):
        fail("device id: 1-32 characters of A-Z a-z 0-9 _ -")

    token = prompt_secret("TTH_DEVICE_TOKEN", "Device token (empty = generate a new one)", False)
    generated = not token
    if generated:
        token = secrets.token_urlsafe(32)  # 32 random bytes -> 43 characters
    if not DEVICE_TOKEN_RE.match(token):
        fail("device token: exactly 43 characters of A-Z a-z 0-9 _ -")
    token_bytes = token.encode("ascii")

    robot = Robot(port, [ssid, password_bytes, token_bytes])
    try:
        robot.wait_until_ready()
        print("robot answered; sending configuration (values are not shown)")
        expect_ok(robot, "prov begin", "begin")
        # Secrets travel hex-encoded: no spaces or terminal line handling to
        # get wrong, and still never printed back.
        expect_ok(robot, "prov sethex ssid " + ssid.hex(), "SSID")
        expect_ok(robot, "prov sethex pass " + password_bytes.hex(), "Wi-Fi password")
        expect_ok(robot, "prov set url " + url, "gateway URL")
        expect_ok(robot, "prov set id " + device_id, "device id")
        expect_ok(robot, "prov sethex token " + token_bytes.hex(), "device token")
        reply = robot.command("prov commit", timeout=6.0)
        if "COMMITTED" not in reply:
            robot.command("prov abort")
            fail(f"commit failed: {reply}\nthe previous configuration is unchanged")
        print(reply)
        if ca_lines:
            send_ca(robot, ca_lines)
        robot.send("prov show")
        for line in robot.collect(1.5):
            print(line)
    finally:
        robot.close()

    digest = hashlib.sha256(token_bytes).hexdigest()
    print()
    print("Gateway registry entry for this robot (the token itself is not shown):")
    print(json.dumps({device_id: {"token_sha256": digest, "activity_id": None, "enabled": True}},
                     indent=2))
    if generated:
        print("A new token was generated and exists only on the robot. If the robot is")
        print("lost or replaced, provision a new token and replace this registry entry.")
    print()
    print("Watch the serial monitor for '[wifi] connected' (pio device monitor -e core2-touch).")


def show(port: str):
    robot = Robot(port, [])
    try:
        robot.wait_until_ready()
        robot.send("prov show")
        for line in robot.collect(1.5):
            print(line)
    finally:
        robot.close()


def ca_only(port: str, ca_lines):
    robot = Robot(port, [])
    try:
        robot.wait_until_ready()
        send_ca(robot, ca_lines)
        robot.send("prov show")
        for line in robot.collect(1.5):
            print(line)
    finally:
        robot.close()


def reset(port: str):
    robot = Robot(port, [])
    try:
        robot.wait_until_ready()
        reply = robot.command("prov reset")
        match = re.search(r"confirm (\d{4})", reply)
        if not match:
            fail(f"unexpected reply: {reply}")
        print("This erases the robot's Wi-Fi credentials, gateway URL and device identity")
        print("(NVS namespace 'tth' only). The robot will go offline until provisioned again.")
        if input("Type ERASE to confirm: ").strip() != "ERASE":
            robot.command("prov reset confirm 0000")  # cancels the pending reset
            fail("cancelled; nothing was erased")
        print(robot.command("prov reset confirm " + match.group(1)))
    finally:
        robot.close()


def main():
    parser = argparse.ArgumentParser(description="Provision a TTH Bot M5Stack Core2 over USB serial.")
    parser.add_argument("--port", help="serial port, for example COM3")
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--show", action="store_true", help="print the redacted configuration")
    mode.add_argument("--reset", action="store_true", help="erase the TTH Bot configuration")
    mode.add_argument("--ca-only", metavar="PEM", help="store only the gateway CA certificate")
    mode.add_argument("--set-wifi", action="store_true", help="change only the Wi-Fi SSID and password")
    mode.add_argument("--set-url", metavar="URL", help="change only the gateway URL")
    parser.add_argument("--ca", metavar="PEM", help="also store this gateway CA certificate")
    args = parser.parse_args()
    if args.ca and (args.show or args.reset or args.ca_only or args.set_wifi or args.set_url):
        fail("--ca is used only with a full provisioning (use --ca-only to change just the CA)")

    if not args.port:
        ports = [p.device + "  " + (p.description or "") for p in serial.tools.list_ports.comports()]
        fail("--port is required. Available ports:\n  " + ("\n  ".join(ports) or "(none found)"))

    if args.show:
        show(args.port)
    elif args.reset:
        reset(args.port)
    elif args.ca_only:
        ca_only(args.port, load_ca(args.ca_only))
    elif args.set_wifi:
        set_wifi(args.port)
    elif args.set_url:
        set_url(args.port, args.set_url)
    else:
        provision(args.port, load_ca(args.ca) if args.ca else None)


if __name__ == "__main__":
    main()
