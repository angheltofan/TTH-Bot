// Prompt parity with the Flutter app (PHASE6_PLAN §5.3).
//
// Fails if:
//   * src/generated/edubot_base_prompt.ts no longer matches
//     lib/core/prompts/edubot_base_prompt.dart (regenerate with
//     `deno task generate`);
//   * the composition pieces in src/prompt.ts drift from
//     lib/features/activities/prompt_composer.dart.
// Runtime never parses Dart; only this test and the generator read it.

import { assert, assertEquals, assertThrows } from "./assert.ts";
import {
  DART_CONSTANT,
  extractDartTripleQuoted,
  renderGenerated,
} from "../scripts/generate_base_prompt.ts";
import { EDUBOT_BASE_PROMPT } from "../src/generated/edubot_base_prompt.ts";
import {
  composeSystemInstruction,
  PARTICIPANTS_PREFIX,
  PARTICIPANTS_SEPARATOR,
  SECTION_SEPARATOR,
} from "../src/prompt.ts";

const DART_PROMPT = new URL("../../lib/core/prompts/edubot_base_prompt.dart", import.meta.url);
const DART_COMPOSER = new URL(
  "../../lib/features/activities/prompt_composer.dart",
  import.meta.url,
);
const GENERATED = new URL("../src/generated/edubot_base_prompt.ts", import.meta.url);

Deno.test("the generated base prompt matches the Dart source exactly", async () => {
  const dart = await Deno.readTextFile(DART_PROMPT);
  const extracted = extractDartTripleQuoted(dart, DART_CONSTANT);
  assertEquals(EDUBOT_BASE_PROMPT, extracted, "run `deno task generate`");
  // And the committed file is byte-identical to what the generator writes.
  const onDisk = (await Deno.readTextFile(GENERATED)).replace(/\r\n/g, "\n");
  assertEquals(onDisk, await renderGenerated(extracted), "run `deno task generate`");
  assert(extracted.includes("Ești TTH Bot"));
});

Deno.test("the composition pieces match the Dart composer", async () => {
  const dart = (await Deno.readTextFile(DART_COMPOSER)).replace(/\r\n/g, "\n");
  // Both sections are trimmed, and joined by a blank line.
  assert(dart.includes("eduBotBasePrompt.trim()"), "base prompt no longer trimmed");
  assert(dart.includes("activity.systemPrompt.trim()"), "activity prompt no longer trimmed");
  assert(dart.includes("sections.join('\\n\\n')"), "section separator changed");
  assertEquals(SECTION_SEPARATOR, "\n\n");
  // The participants sentence: adjacent literals before the interpolation.
  const interpolation = "'${activity.participants.join(', ')}.'";
  const at = dart.indexOf(interpolation);
  assert(at > 0, "participants interpolation changed");
  const block = dart.slice(dart.lastIndexOf("sections.add(", at), at);
  const prefix = [...block.matchAll(/'([^'$]*)'/g)].map((m) => m[1]).join("");
  assertEquals(PARTICIPANTS_PREFIX, prefix);
  assertEquals(PARTICIPANTS_SEPARATOR, ", ");
});

Deno.test("composition follows the Dart rules", () => {
  const withNames = composeSystemInstruction({
    prompt: "  Activitate: test.  ",
    participants: ["Maria", "Ștefan"],
  });
  assertEquals(
    withNames,
    `${EDUBOT_BASE_PROMPT.trim()}\n\nActivitate: test.\n\n${PARTICIPANTS_PREFIX}Maria, Ștefan.`,
  );
  const without = composeSystemInstruction({ prompt: "Liber.", participants: [] });
  assertEquals(without, `${EDUBOT_BASE_PROMPT.trim()}\n\nLiber.`);
});

Deno.test("the generator refuses Dart constructs it does not translate", () => {
  const ok = "const String p = '''\nhello\n''';";
  assertEquals(extractDartTripleQuoted(ok, "p"), "hello\n");
  assertThrows(() => extractDartTripleQuoted("const String p = '''a $b''';", "p"));
  assertThrows(() => extractDartTripleQuoted("const String p = '''a \\n b''';", "p"));
  assertThrows(() => extractDartTripleQuoted("const String p = '''a''' 'b';", "p"));
  assertThrows(() => extractDartTripleQuoted("const String q = '''a''';", "p"));
  assertThrows(() =>
    extractDartTripleQuoted("const String p = '''a''';\nconst String p = '''b''';", "p")
  );
});
