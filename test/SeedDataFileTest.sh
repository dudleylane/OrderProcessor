#!/usr/bin/env bash
# SeedDataFileTest.sh: seedData --file loads a file's instruments, accounts and clearing firms, and none of the test set
# (#585). It adds what isn't stored and skips what's stored as the file gives it. A file that's wrong, or that would
# change what's stored, is refused whole: seedData exits 1 and writes nothing.
#
# usage: SeedDataFileTest.sh <seedData>

set -u
SEED="${1:?usage: $0 <seedData>}"
WORK="$(mktemp -d)"
trap 'rm -rf -- "${WORK:?}"' EXIT
export EXCHANGE_LOG="$WORK/exchange.log"

failures=0
check() {
    if [[ "$2" == "$3" ]]; then
        echo "ok    $1"
    else
        echo "FAIL  $1: got '$2', wanted '$3'"
        failures=$((failures + 1))
    fi
}

cat > "$WORK/reference.json" <<'EOF'
{
  "instruments": [
    { "symbol": "ACME", "securityId": "US0000000001", "securityIdSource": "ISIN" },
    { "symbol": "WIDG", "securityId": "US0000000002", "securityIdSource": "ISIN" }
  ],
  "accounts": [
    { "account": "DESK-1", "firm": "Acme Capital", "type": "PRINCIPAL" },
    { "account": "CLIENT-9", "firm": "Widget Fund", "type": "AGENCY" }
  ],
  "clearingFirms": [
    { "firm": "ACME-CLEARING" }
  ]
}
EOF

# 1. A file that's refused writes nothing: the directory isn't even made
sed 's/"AGENCY"/"agency"/' "$WORK/reference.json" > "$WORK/wrong.json"
out="$("$SEED" --data-dir "$WORK/data" --file "$WORK/wrong.json" 2>&1)"
status=$?
check "a wrong file: exit status" "$status" 1
check "... names the entry and why" \
    "$(grep -c 'accounts\[1\] (CLIENT-9): "type" must be PRINCIPAL or AGENCY$' <<<"$out")" 1
check "... and makes no directory" "$([[ -e "$WORK/data" ]] && echo yes || echo no)" no

# 2. A new directory gets the file's entries, and none of the test set
out="$("$SEED" --data-dir "$WORK/data" --file "$WORK/reference.json" 2>&1)"
status=$?
check "a new directory: exit status" "$status" 0
check "... gets each of the file's entries" "$(grep -c '^  Added' <<<"$out")" 5
check "... and says so" \
    "$(grep -cx "Seeded 2 instruments, 2 accounts and 1 clearing firms into $WORK/data from $WORK/reference.json" \
        <<<"$out")" 1
check "... and none of the test set" "$(grep -c 'AAPL\|TRADING-1\|HOUSE-CLEARING' <<<"$out")" 0

# 3. Again: every entry is stored with the fields the file gives it, so each is skipped. A field the directory didn't
# keep would be refused here, as a change
out="$("$SEED" --data-dir "$WORK/data" --file "$WORK/reference.json" 2>&1)"
status=$?
check "the same file again: exit status" "$status" 0
check "... skips every entry" "$(grep -c 'already exists, skipping$' <<<"$out")" 5
check "... and adds none" "$(grep -c '^  Added' <<<"$out")" 0

# 4. A file that would change what's stored is refused whole, the instrument it would add included
cat > "$WORK/changed.json" <<'EOF'
{
  "instruments": [
    { "symbol": "WIDG", "securityId": "US0000000003", "securityIdSource": "ISIN" },
    { "symbol": "NEWX", "securityId": "US0000000004", "securityIdSource": "ISIN" }
  ],
  "accounts": [
    { "account": "DESK-1", "firm": "Acme Capital", "type": "AGENCY" }
  ]
}
EOF
out="$("$SEED" --data-dir "$WORK/data" --file "$WORK/changed.json" 2>&1)"
status=$?
check "a file that would change what's stored: exit status" "$status" 1
check "... names the instrument" "$(grep -cx 'seedData: instrument WIDG is stored with securityId US0000000002, securityIdSource ISIN, and the file gives securityId US0000000003, securityIdSource ISIN' <<<"$out")" 1
check "... and the account" "$(grep -cx 'seedData: account DESK-1 is stored with firm Acme Capital, type PRINCIPAL, and the file gives firm Acme Capital, type AGENCY' <<<"$out")" 1
check "... and adds nothing" "$(grep -c '^  Added' <<<"$out")" 0

# 5. The directory is as the first file left it, without the refused file's new instrument
out="$("$SEED" --data-dir "$WORK/data" --file "$WORK/reference.json" 2>&1)"
check "... where the first file still finds every entry as it gives it" \
    "$(grep -c 'already exists, skipping$' <<<"$out")" 5
cat > "$WORK/new.json" <<'EOF'
{ "instruments": [ { "symbol": "NEWX", "securityId": "US0000000004", "securityIdSource": "ISIN" } ] }
EOF
out="$("$SEED" --data-dir "$WORK/data" --file "$WORK/new.json" 2>&1)"
check "... and the refused file's new instrument isn't stored" "$(grep -c '^  Added instrument: NEWX ' <<<"$out")" 1

# 6. --if-empty leaves a directory with instruments alone, and seeds a new one from the file
out="$("$SEED" --if-empty --data-dir "$WORK/data" --file "$WORK/reference.json" 2>&1)"
check "--if-empty and a file, on a directory with instruments" "$(grep -c "isn't seeded (--if-empty)" <<<"$out")" 1
out="$("$SEED" --if-empty --data-dir "$WORK/other" --file "$WORK/reference.json" 2>&1)"
check "--if-empty and a file, on a new directory" "$(grep -c '^Seeded 2 instruments' <<<"$out")" 1

# 7. A file it can't open
out="$("$SEED" --data-dir "$WORK/data" --file "$WORK/missing.json" 2>&1)"
status=$?
check "a missing file: exit status" "$status" 1
check "... names it" "$(grep -cx "seedData: $WORK/missing.json: can't open it" <<<"$out")" 1

(( failures == 0 )) || exit 1
