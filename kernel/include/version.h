/*
 * version.h - AuroraOS version management
 *
 * Provides version information for the kernel and build system.
 * Version format: MAJOR.MINOR.PATCH
 *
 * The build system auto-generates BUILD_DATE and GIT_HASH from
 * the build environment. If not available, defaults are used.
 */
#ifndef VERSION_H
#define VERSION_H

#define AURORAOS_MAJOR      3
#define AURORAOS_MINOR      9
#define AURORAOS_PATCH      2

#define AURORAOS_VERSION    "AuroraOS v3.9.2"

/* These are set by the Makefile via -D flags */
#ifndef BUILD_DATE
#define BUILD_DATE          __DATE__
#endif

#ifndef GIT_HASH
#define GIT_HASH            "unknown"
#endif

#ifndef BUILD_TYPE
#define BUILD_TYPE          "release"
#endif

/* Full version string for display */
#define AURORAOS_FULL_VERSION   AURORAOS_VERSION " (" BUILD_TYPE ", " BUILD_DATE ")"

/* GitHub repository URL for update checks */
#define AURORAOS_REPO_URL       "https://github.com/zhan1206/aurora-os"
#define AURORAOS_REPO_API       "https://api.github.com/repos/zhan1206/aurora-os/releases/latest"

#endif /* VERSION_H */