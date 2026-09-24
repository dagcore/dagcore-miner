# Turns the Linux config.env.example into the one shipped with the Windows
# build (make windows -> build/win/config.env.example). Only what differs is
# touched; the Makefile fails the build if a Linux path survives, so a change
# to the wording below in config.env.example has to be mirrored here.

/^# Installed to \/etc\/dagcore-miner\/config.env\./,/^# overwritten by an upgrade\./c\
# Windows: save this file as config.env in the miner's folder, next to\
# dagcore-miner.exe. The miner looks for it there first, whichever folder it\
# is started from; a save from the dashboard creates it there if it is missing.

s|/var/lib/dagcore-miner/overrides.env|overrides.env, next to the .exe,|

/^GPU_CORE_CLOCK_BASE=0/i\
# Windows: not applied yet - this build has no NVML, so clock locks and the\
# offsets below are ignored.

/^# Cache file\. Empty = \$XDG_CACHE_HOME/,/^# to \$HOME\/\.cache\/dagcore-miner\/autotune\.json\./c\
# Cache file. Empty = autotune.json next to the .exe.

/^# Directory holding the dashboard's index\.html\./,/^DASHBOARD_DIR=/c\
# Directory holding the dashboard's index.html. Not needed on Windows: the\
# dashboard folder next to the .exe is found by itself. Set it only to serve\
# a copy kept somewhere else.\
#DASHBOARD_DIR=
