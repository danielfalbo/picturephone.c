#! /bin/bash

TEMP_FILE=$(mktemp)

trap "rm -f '$TEMP_FILE'" EXIT

{
  echo "see README, Makefile and picturephone.c"
  echo "<kilo.c for reference>"
  curl "https://raw.githubusercontent.com/antirez/kilo/master/kilo.c"
  echo "</ kilo.c for reference>"

} > "$TEMP_FILE"

cat "$TEMP_FILE" | pbcopy
echo "Success: Content copied to clipboard."
