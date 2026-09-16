#!/usr/bin/env bash
# Fails if a tracked (or untracked, non-ignored) file contains something that
# looks like a secret (PHASE6_PLAN §4.1). Run from the repository root:
#
#   bash scripts/check_no_secrets.sh
#
# The Supabase *publishable* key (sb_publishable_…) is public by design and
# is not flagged. Example/env templates are checked for non-empty secret
# values, which must stay empty.
set -euo pipefail
cd "$(git rev-parse --show-toplevel)"

patterns=(
  'AIza[0-9A-Za-z_-]{35}'                                   # Google API key
  'sb_secret_[0-9A-Za-z_-]{10,}'                            # Supabase secret key
  'eyJ[A-Za-z0-9_-]{10,}\.eyJ[A-Za-z0-9_-]{10,}\.[A-Za-z0-9_-]{10,}' # JWT (service role etc.)
  '-----BEGIN ([A-Z]+ )?PRIVATE KEY-----'                   # private keys
  '(GEMINI_API_KEY|GATEWAY_MINT_SECRET)=[A-Za-z0-9_/+=-]{24,}([^:A-Za-z0-9_/+=-]|$)' # a real-looking assigned secret (placeholders like <...> or NAME:latest are not)
  'auth_tokens/[A-Za-z0-9_-]{16,}'                          # Gemini ephemeral tokens
)

files=$(git ls-files --cached --others --exclude-standard | grep -v -E '(^|/)(build|\.dart_tool|\.pio|node_modules)/' || true)
status=0
for pattern in "${patterns[@]}"; do
  # shellcheck disable=SC2086
  hits=$(printf '%s\n' $files | xargs -r grep -I -n -E -- "$pattern" 2>/dev/null \
    | grep -v -E 'scripts/check_no_secrets\.sh' || true)
  if [ -n "$hits" ]; then
    echo "possible secret matching: $pattern"
    echo "$hits" | sed -E 's/(.{0,160}).*/  \1/'
    status=1
  fi
done

if [ "$status" -eq 0 ]; then
  echo "no secrets found in $(printf '%s\n' $files | wc -l) files"
fi
exit "$status"
