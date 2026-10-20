// Identifies the source snapshot a binary was built from.
//  * Git checkouts: the Xbox 360 project runs tools/xbox360/source_stamp.bat
//    before each build (B360_HAS_SOURCE_STAMP), which writes source_stamp.h
//    with B360_GIT_COMMIT / B360_GIT_DATE; the Makefile passes them directly.
//  * GitHub's "Download ZIP" (git archive) expands the $Format$ placeholders
//    below via .gitattributes (export-subst).
#ifndef B360_BUILD_ID_H
#define B360_BUILD_ID_H

#if defined(B360_HAS_SOURCE_STAMP)
#include "source_stamp.h"
#endif

#define B360_SOURCE_COMMIT "$Format:%h$"
#define B360_SOURCE_DATE "$Format:%cI$"

#endif
