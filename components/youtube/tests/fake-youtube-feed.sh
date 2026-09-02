#!/bin/sh
set -eu
test "$#" -eq 3
case $1 in
	search|channel) ;;
	*) exit 64 ;;
esac
root=${RG_YOUTUBE_HOME_TEST_ROOT:?}
offset=$3
case $offset in
	''|*[!0-9]*) exit 64 ;;
esac
emit_item()
{
	index=$1
	id=$(printf 'HomeCard%03d' "$index")
	printf 'ITEM\t%s\tReal card %s\tChannel %s\t%s days ago\t%s\thttps://i.ytimg.com/vi/%s/mqdefault.jpg\thttps://www.youtube.com/watch?v=%s\t\n' \
		"$id" "$index" "$index" "$index" "$((60 + index))" "$id" "$id"
}

index=$((offset + 1))
end=$((offset + 8))
while test "$index" -le "$end"; do
	emit_item "$index"
	index=$((index + 1))
done
printf 'BATCH\t8\t8\tmore=YES\n'
# Metadata must become visible before the page completion/thumbnail phase.
sleep 0.05
next=$((offset + 8))
if test "$next" -ge 96; then next=END; fi
printf 'DONE\t8\t%s\tcache=STALE\tnext=%s\n' "$2" "$next"
# Give the unit test a deterministic window to prove that metadata is visible
# before thumbnails complete.
sleep 0.02
index=$((offset + 1))
while test "$index" -le "$end"; do
	id=$(printf 'HomeCard%03d' "$index")
	printf 'THUMB\t%s\t%s/%s.jpg\n' "$id" "$root" "$id"
	index=$((index + 1))
done
printf 'THUMBS\t8\n'
