#! /bin/bash

TEMP_FILE=$(mktemp)

trap "rm -f '$TEMP_FILE'" EXIT

{
  echo "<picturephone.c>"
  cat "picturephone.c"
  echo "</picturephone.c>"

  echo "<Makefile>"
  cat "Makefile"
  echo "</Makefile>"

  echo "<README>"
  cat "README"
  echo "</README>"
} > "$TEMP_FILE"

cat "$TEMP_FILE" | pbcopy
echo "Success: Content copied to clipboard."
