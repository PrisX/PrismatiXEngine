# Packaged Player user data

The packaged Player treats its installation directory as game content, not as
a writable save directory. It keeps `Save/config.dat`, `Save/profile.dat`,
`Save/save_<slot>.pxsav`, `Save/autosave.pxsav`, and `logs/PrismatiXPlayer.log`
under a per-user directory chosen by SDL's `SDL_GetPrefPath`. The organization
is `PrismatiXEngine`; the application directory is `Game ` followed by the
SHA-256 of the package's stable `gameId`. It does not change when content is
repacked or the installation is moved.

To choose another parent directory, set `PRISMATIX_PLAYER_DATA_ROOT` to an
**absolute** path before starting Player. Player still creates a separate
`Game <SHA-256(gameId)>` child within that parent. This is useful for isolated
tests or portable deployments; put it outside an artifact/distribution output
if you want the ownership-safe CLI to update that output in place. An invalid
or unwritable data location fails startup rather than silently falling back to
the package or launcher working directory. Developer launches without an
adjacent package keep their existing relative `Save/` and `logs/` behavior.

On the first launch with this storage policy, Player copies recognized files
from the older `<package>/Save/` directory to the new location. It recognizes
settings, profile, numeric slots, autosave, and their `.legacy*.bak` key
migration backups. Existing destination files win; nothing in the package is
overwritten or deleted. A marker in the new user-data directory prevents a
later launch from restoring stale old saves after a player deletes or changes
them. A failed or unsafe import stops startup and retains the old files for
recovery. Persistence file path strings are interpreted as UTF-8, including
Windows paths with non-ASCII user names.

The old portable `Save/` and `logs/` directories remain beside an installation
that was already run with an older Player. Their bytes are preserved, so an
in-place `prismatix pack` may still reject that *old* distribution as modified.
After verifying the imported saves, move those legacy directories to a backup
outside the distribution before repacking. New packages no longer create them
by default. `PrismatiXPlayer --inspect-save <path> --package <Package/manifest.json>`
continues to accept an explicit save path.
