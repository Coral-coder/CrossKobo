/* miniz is built as a static library inside this project, so the export
   macros CMake would normally generate are empty. */
#ifndef MINIZ_EXPORT_H
#define MINIZ_EXPORT_H
#define MINIZ_EXPORT
#define MINIZ_NO_EXPORT
#endif
