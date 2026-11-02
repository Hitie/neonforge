# GitHub build automation

The Windows workflow runs on pushes to main, pull requests targeting main and manual dispatch. It uses a GitHub-hosted Windows runner to configure CMake, build x64 Release, run the standalone tests, package the SDK/source and retain downloads with SHA-256 hashes for 14 days.

No SSH key, deploy key or GPG private key is configured or required by this workflow. Checkout uses GitHub's supplied repository token with read-only contents permission and does not persist Git credentials. The three official actions are pinned to verified upstream commit SHAs; version comments identify the researched releases.

Open the repository's Actions page, choose a successful Windows run, and download its SDK/source artifact. Test evidence is retained separately, including on failed runs when available. Manual dispatch becomes available after the workflow is on the repository's default branch.

This workflow builds the standalone developer SDK. It does not install Cyberpunk, run the game, prove engine hooks or publish a public release automatically. The exact downloaded artifact must retain its development status and game-feature limitations.

Future releases can add a tag/release workflow after the corresponding game capabilities are qualified.

## Security automation

The separate CodeQL workflow analyzes C/C++ with the actual Windows CMake build and Python without a build. It runs on main, pull requests, weekly and on manual dispatch. Only the analysis job receives security-events write permission to upload findings. This does not grant repository contents write access. CodeQL results appear under the repository Security tab after a successful scan.

Dependabot checks pinned GitHub Actions weekly and proposes updates for review. It does not auto-merge them. Repository settings such as branch protection, secret scanning, push protection and private vulnerability reporting must also be enabled and verified in GitHub; configuration files alone do not activate those controls.
