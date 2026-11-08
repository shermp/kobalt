#include <fontconfig/fontconfig.h>
#include <stdio.h>

// Run through the normal supervisor to inspect its actual, sanitized font setup.
int main(void)
{
    FcConfig *config = FcInitLoadConfigAndFonts();
    if (!config)
        return 1;
    FcStrList *files = FcConfigGetConfigFiles(config);
    FcChar8 *path;
    while ((path = FcStrListNext(files)))
        printf("CONFIG\t%s\n", path);
    FcStrListDone(files);
    FcPattern *pattern = FcPatternCreate();
    FcObjectSet *objects = FcObjectSetBuild(FC_FILE, FC_FAMILY, NULL);
    FcFontSet *fonts = FcFontList(config, pattern, objects);
    int found = fonts && fonts->nfont;
    if (fonts) {
        for (int index = 0; index < fonts->nfont; ++index) {
            FcChar8 *file = NULL, *family = NULL;
            if (FcPatternGetString(fonts->fonts[index], FC_FILE, 0, &file) == FcResultMatch
                && FcPatternGetString(fonts->fonts[index], FC_FAMILY, 0, &family) == FcResultMatch)
                printf("FONT\t%s\t%s\n", file, family);
        }
        FcFontSetDestroy(fonts);
    }
    FcObjectSetDestroy(objects);
    FcPatternDestroy(pattern);
    const char *families[] = { "sans-serif", "serif", "monospace" };
    for (unsigned index = 0; index < sizeof(families) / sizeof(families[0]); ++index) {
        pattern = FcNameParse((const FcChar8 *)families[index]);
        FcConfigSubstitute(config, pattern, FcMatchPattern);
        FcDefaultSubstitute(pattern);
        FcResult result;
        FcPattern *match = FcFontMatch(config, pattern, &result);
        FcChar8 *file = NULL;
        if (!match || FcPatternGetString(match, FC_FILE, 0, &file) != FcResultMatch)
            found = 0;
        else
            printf("MATCH\t%s\t%s\n", families[index], file);
        if (match)
            FcPatternDestroy(match);
        FcPatternDestroy(pattern);
    }
    FcConfigDestroy(config);
    FcFini();
    return found ? 0 : 1;
}
