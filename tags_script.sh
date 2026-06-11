find . -maxdepth 1 -type f \( \
    -iname "*.jpg" -o \
    -iname "*.jpeg" -o \
    -iname "*.png" -o \
    -iname "*.webp" -o \
    -iname "*.gif" -o \
    -iname "*.avif" -o \
    -iname "*.heic" -o \
    -iname "*.tif" -o \
    -iname "*.tiff" -o \
    -iname "*.mp4" -o \
    -iname "*.mov" -o \
    -iname "*.m4v" -o \
    -iname "*.webm" -o \
    -iname "*.mkv" \
\) -print0 | while IFS= read -r -d '' f; do

    base=$(basename "$f")
    json="meta/$base.json"

    [ -f "$json" ] || continue

    date=$(jq -r '.date' "$json" | sed 's/-/:/g')

    args=()

    for tag in $(jq -r '.tags' "$json"); do
        args+=("-Keywords+=$tag")
        args+=("-Subject+=$tag")
    done

    exiftool -overwrite_original \
        -Title="${base%.*}" \
        -Genre="$tag" \
        -DateTimeOriginal="$date" \
        -CreateDate="$date" \
        -Description="$(jq -r '.source' "$json")" \
        -Artist="$(jq -r '.creator_id' "$json")" \
        -Copyright="$(jq -r '.file_url' "$json")" \
        -XMP:Identifier="$(jq -r '.id' "$json")" \
        -XMP:Label="$tag" \
        -XMP:Nickname="$(jq -r '.filename' "$json")" \
        -XMP:Rating="$(jq -r '.score' "$json")" \
        -Comment="$(cat "$json")" \
        "${args[@]}" \
        "$f"

done

l
find . -maxdepth 1 -type f \( \
    -iname "*.jpg" -o \
    -iname "*.jpeg" -o \
    -iname "*.png" -o \
    -iname "*.webp" -o \
    -iname "*.gif" -o \
    -iname "*.avif" -o \
    -iname "*.heic" -o \
    -iname "*.tif" -o \
    -iname "*.tiff" -o \
    -iname "*.mp4" -o \
    -iname "*.mov" -o \
    -iname "*.m4v" -o \
    -iname "*.webm" -o \
    -iname "*.mkv" \
\) -print0 | while IFS= read -r -d '' f; do

    base=$(basename "$f")
    json="meta/$base.json"

    [ -f "$json" ] || continue

    date=$(jq -r '.date' "$json" | sed 's/-/:/g')

    title="${base%.*}"
    source=$(jq -r '.source' "$json")
    creator=$(jq -r '.creator_id' "$json")
    file_url=$(jq -r '.file_url' "$json")
    id=$(jq -r '.id' "$json")
    filename=$(jq -r '.filename' "$json")
    score=$(jq -r '.score' "$json")

    args=()

    while IFS= read -r tag; do
        [ -n "$tag" ] || continue

        args+=("-Keywords+=$tag")
        args+=("-Subject+=$tag")
        args+=("-XMP:Subject+=$tag")
        args+=("-IPTC:Keywords+=$tag")
        args+=("-Genre+=$tag")
    done < <(jq -r '.tags[]?' "$json")

    exiftool -overwrite_original \
        -Title="$title" \
        -ObjectName="$title" \
        -XPTitle="$title" \
        -Description="$source" \
        -ImageDescription="$source" \
        -Comment="$(cat "$json")" \
        -XPComment="$source" \
        -Artist="$creator" \
        -Author="$creator" \
        -Creator="$creator" \
        -Copyright="$file_url" \
        -DateTimeOriginal="$date" \
        -CreateDate="$date" \
        -ModifyDate="$date" \
        -XMP:Identifier="$id" \
        -XMP:Nickname="$filename" \
        -XMP:Rating="$score" \
        "${args[@]}" \
        "$f"

done