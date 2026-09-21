# Security Policy

## Supported version

Security fixes target the main development branch only. There are no backports
to old branches. The only supported game-data version is 1.8.1g.

## Reporting a vulnerability

Do not disclose a vulnerability in a public issue. Use GitHub private
vulnerability reporting. If it is unavailable, request a private contact method
through the repository owner's GitHub profile before sending exploit details.

Include the affected commit, platform, impact, and smallest practical
reproduction. Do not attach game files, provisioning profiles, certificates,
device identifiers, personal paths, or unredacted runtime logs.

## Scope

Security-sensitive areas include guest-memory bounds, ELF and ZIP parsing, file
path mapping, executable-page permissions, the iOS sandbox, and code signing.
The original game, Gameloft services, and the current Unity client are outside
this project's security scope.
