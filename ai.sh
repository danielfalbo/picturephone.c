#! /bin/bash

TEMP_FILE=$(mktemp)

trap "rm -f '$TEMP_FILE'" EXIT

{
  echo "<picturephone.c>"
  cat "picturephone.c"
  echo "</ picturephone.c>"

  echo "<Makefile>"
  cat "Makefile"
  echo "</ Makefile>"

  echo "<README>"
  cat "README"
  echo "</ README>"

  echo "<kilo.c for reference>"
  curl "https://raw.githubusercontent.com/antirez/kilo/master/kilo.c"
  echo "</ kilo.c for reference>"

  echo "<ertdfgcvb/play.core/src/programs/camera/camera_gray.js for reference>"
  curl "https://raw.githubusercontent.com/ertdfgcvb/play.core/master/src/programs/camera/camera_gray.js"
  echo "</ ertdfgcvb/play.core/src/programs/camera/camera_gray.js for reference>"

} > "$TEMP_FILE"

cat "$TEMP_FILE" | pbcopy
echo "Success: Content copied to clipboard."
