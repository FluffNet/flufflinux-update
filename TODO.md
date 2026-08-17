# Fluff Linux Update — follow-up work

## Tomorrow

- Add a dedicated file-conflict error for pacman failures such as `exists in filesystem`. Explain that files outside pacman's ownership have been placed or modified on the system and require manual attention; do not report this as the generic update-size error.
- Design and test conflict recovery for obsolete, duplicate, or broken non-system packages. If a conflicting package is not on Fluff Linux's protected system-package list, remove it with `pacman -Rdd` even when no replacement can be identified; broken optional software must not hold back the system upgrade. Protected system packages must stop for explicit handling instead.
