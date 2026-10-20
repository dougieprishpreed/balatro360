// Identifies the source snapshot a binary was built from. GitHub's
// "Download ZIP" (git archive) expands the $Format$ placeholders below via
// .gitattributes (export-subst); a plain git checkout leaves them as-is.
#ifndef B360_BUILD_ID_H
#define B360_BUILD_ID_H

#define B360_SOURCE_COMMIT "$Format:%h$"
#define B360_SOURCE_DATE "$Format:%cI$"

#endif
