#!/bin/sh
# Synthetic spine items plus unchanged, checked-in EPUB resources.
set -eu
test "$#" -eq 1 || { echo "Usage: $0 NEW-DIRECTORY" >&2; exit 2; }
root=$(CDPATH= cd -- "$(dirname "$0")/../../.." && pwd)
out=$1
mkdir "$out"
for workload in short long fonts images rtl links image-edges textless-images; do
    dir=$out/$workload
    mkdir "$dir"
    manifest=$dir/resources.txt
    image_workload=0
    case "$workload" in
        long) rows=512 ;;
        images|image-edges|textless-images) rows=0; image_workload=1 ;;
        *) rows=24 ;;
    esac
    if [ "$workload" = fonts ]; then
        cp "$root/tests/fixtures/AmstelvarAlpha-VF.ttf" "$dir/serif.ttf"
        cp "$root/assets/fonts/NotoSerif-Italic.ttf" "$dir/italic.ttf"
        cat >"$dir/fonts.css" <<'CSS'
@font-face { font-family: Fixture; src: url(serif.ttf); }
@font-face { font-family: Fixture; font-style: italic; src: url(italic.ttf); }
body { font-family: Fixture, serif; }
CSS
        printf 'serif.ttf font/ttf\nitalic.ttf font/ttf\nfonts.css text/css\n' >>"$manifest"
    fi
    if [ "$workload" = rtl ]; then
        cp "$root/assets/fonts/NotoNaskhArabic-Regular.ttf" "$dir/arabic.ttf"
        printf '@font-face { font-family: Fixture; src: url(arabic.ttf); } body { font-family: Fixture, serif; }\n' >"$dir/fonts.css"
        printf 'arabic.ttf font/ttf\nfonts.css text/css\n' >>"$manifest"
    fi
    if [ "$image_workload" -eq 1 ]; then
        unzip -p "$root/tests/fixtures/trevelyans-type-tester.kepub.epub" OEBPS/cover.png >"$dir/cover.png"
        unzip -p "$root/tests/fixtures/trevelyans-type-tester.kepub.epub" OEBPS/ornament.png >"$dir/ornament.png"
        unzip -p "$root/tests/fixtures/reporter-figure-float.epub" Images/img_002.png >"$dir/reporter.png"
    fi
    if [ "$workload" = image-edges ]; then
        # A truncated PNG passes the signature check. An unsupported resource
        # is omitted from the manifest, just as the reader declines to serve it.
        printf '\211PNG\r\n\032\n' >"$dir/broken.png"
        printf '%s\n' 'broken.png image/png' >>"$manifest"
    fi
    for spine in 0 1 2 3; do
        chapter=chapter$spine.xhtml
        awk -v spine="$spine" -v workload="$workload" -v rows="$rows" '
            BEGIN {
                rtl = workload == "rtl"
                edges = workload == "image-edges"
                textless = workload == "textless-images"
                images = workload == "images" || edges || textless
                fonts = workload == "fonts" || rtl
                printf "<?xml version=\"1.0\"?><html xmlns=\"http://www.w3.org/1999/xhtml\" "
                printf "xmlns:epub=\"http://www.idpf.org/2007/ops\"%s><head><title>Chapter %d</title>",                     rtl ? " dir=\"rtl\" lang=\"ar\"" : "", spine + 1
                if (fonts) print "<link rel=\"stylesheet\" href=\"fonts.css\"/>"
                print "</head><body>"
                for (section = 0; section < (images ? 8 : 3); ++section) {
                    if (section) print "<section style=\"break-before:column\">"
                    else print "<section>"
                    if (!textless || (spine && section == 3))
                        printf "<h1>Chapter %d / %d</h1>\n", spine + 1, section + 1
                    if (images)
                        printf "<img src=\"image-%d-%d.png\" alt=\"Fixture illustration\"/>\n", spine, section
                    if (edges && section == 1) print "<img src=\"broken.png\" alt=\"Broken fixture image\"/>"
                    if (edges && section == 4) print "<img src=\"unsupported.bin\" alt=\"Unsupported fixture image\"/>"
                    for (row = 0; row < rows; ++row) {
                        if (rtl) printf "<p>هذا نص عربي لاختبار تخطيط الصفحة %d. <bdi>שלום 𐐷</bdi></p>\n", row + 1
                        else if (workload == "fonts") print "<p>Embedded serif text with <em>embedded italics</em> and a Unicode ellipsis…</p>"
                        else if (workload == "links")
                            printf "<p><a href=\"chapter%d.xhtml#note\">Chapter note %d</a> <a href=\"#note\" epub:type=\"noteref\">Footnote</a></p>\n", (spine + 1) % 4, row + 1
                        else print "<p>Reading on the Forma with retained columns and source positions.</p>"
                    }
                    print "</section>"
                }
                if (workload == "links") print "<aside id=\"note\" epub:type=\"footnote\">A fixture footnote.</aside>"
                print "</body></html>"
            }' >"$dir/$chapter"
        printf '%s application/xhtml+xml\n' "$chapter" >>"$manifest"
        if [ "$image_workload" -eq 1 ]; then
            for section in 0 1 2 3 4 5 6 7; do
                case $(((spine + section) % 3)) in
                    0) picture=cover ;;
                    1) picture=ornament ;;
                    2) picture=reporter ;;
                esac
                image=image-$spine-$section.png
                cp "$dir/$picture.png" "$dir/$image"
                printf '%s image/png\n' "$image" >>"$manifest"
            done
        fi
    done
done
