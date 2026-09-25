DagCore Miner for Linux - getting started
=========================================

An OpenCL miner for BlockDAG, chain 1404. It mines on NVIDIA, AMD and Intel
graphics cards, or on the processor alone, and it is set up for the DagCore
pool (stratum.dagcore.net, port 3334) out of the box.

This kit runs from its own folder: nothing to compile, nothing to install.
For a rig that should start mining at boot, see "Running as a service" at the
end.

Read this file in a terminal with:  less README.md


What is in this folder
----------------------

    dagcore-miner        the miner: graphics card and processor (OpenCL)
    dagcore-miner-cpu    processor only; needs no graphics driver
    dagcore_gpu.cl       the program the graphics card runs; keep it next to
                         dagcore-miner, which loads it from there
    dashboard/           the web dashboard the miner serves
    config.env.example   every setting, each one explained where it stands
    LICENSE              the MIT licence the miner is released under
    SHA256SUMS           the checksum of every other file in this folder
    README.md            this file


1. Check the files
------------------

From inside this folder:

    sha256sum -c SHA256SUMS

Every line must end in OK. FAILED means that file is damaged or is not the
one released: delete the kit and download it again.

The sums travel in the same archive as the files, so they catch a damaged or
incomplete download, not a deliberately altered archive. The release page on
GitHub shows a sha256 for the archive itself.


2. What you need
----------------

- Linux on x86-64, with glibc 2.34 or newer: Ubuntu 22.04, Debian 12, or a
  later release of either.
- A BDAG wallet address: 0x followed by 40 hexadecimal characters.
- For mining on a graphics card: the card maker's driver with OpenCL, and
  the OpenCL loader libOpenCL.so.1 (Debian and Ubuntu: ocl-icd-libopencl1).
    NVIDIA   the NVIDIA driver includes OpenCL
    AMD      ROCm, or another OpenCL driver for your card
    Intel    the Intel compute runtime (Debian and Ubuntu: intel-opencl-icd)

  "clinfo -l" lists every card OpenCL can see. If yours is not there, the
  miner will not see it either, and the fix is the driver, not the miner.

The processor-only build, dagcore-miner-cpu, needs none of that.


3. Make config.env
------------------

The miner reads config.env from its own folder first. Make it from the
example:

    cp config.env.example config.env

Then change two lines in it, with any editor (nano config.env):

    WALLET=0xYOURADDRESS
    DASHBOARD_DIR=/the/full/path/of/this/folder/dashboard

Or change both at once, from inside this folder:

    sed -i -e 's|^WALLET=.*|WALLET=0xYOURADDRESS|' \
           -e "s|^DASHBOARD_DIR=.*|DASHBOARD_DIR=$PWD/dashboard|" config.env

Replace 0xYOURADDRESS with your own address, and check it twice: a typo
mines for somebody else, and nothing can undo that.

DASHBOARD_DIR has to be changed because the example points at
/opt/dagcore-miner, where the installer puts the dashboard. Left as it is,
the miner mines but serves no dashboard.

The pool and the port are already DagCore's. For the processor-only build,
also set THREADS=-1 (half the logical cores) or a number of threads; left at
0, it warns and uses -1 anyway.

Every other line can stay as it is. What each one does is written above it
in the file.


4. Start it
-----------

From inside this folder:

    DAGCORE_TOKEN_FILE=$PWD/api-token \
    DAGCORE_OVERRIDES_FILE=$PWD/overrides.env ./dagcore-miner

or, for the processor only, the same with ./dagcore-miner-cpu at the end.

The two variables keep the dashboard's control token and its saved tuning in
this folder. Without them the miner looks for both in /etc/dagcore-miner and
/var/lib/dagcore-miner, which a normal user cannot write: it still mines,
but the dashboard can only show, not change anything.

The miner itself is simply ./dagcore-miner. Any setting can also be given on
the command line, where it wins over config.env; this runs without a
config.env at all, with the same two variables in front:

    DAGCORE_TOKEN_FILE=$PWD/api-token \
    DAGCORE_OVERRIDES_FILE=$PWD/overrides.env \
        ./dagcore-miner --wallet 0xYOURADDRESS --dashboard-dir "$PWD/dashboard"

Without them it mines just the same, but prints "cannot write
/etc/dagcore-miner/api-token ... control API disabled" and the dashboard
only shows.

./dagcore-miner --help lists every option.

How to tell it works. On start, the GPU build names the card it uses:

    [DagCore GPU] GPU 0: <card name> (platform 0, device 0)

then the log shows "Connected!", "Authorized" and "Mining started!", and soon
lines with "Share ACCEPTED". In the first minutes they come fast: the pool
starts every connection at a low difficulty and raises it.

No "Authorized", and instead "Share REJECTED" with "username should be a
valid evm address" or "unauthorized", means the pool refused the wallet:
the miner keeps running, but nothing it finds counts. Stop it and check
WALLET in config.env - still 0xYOURADDRESS, a character missing, or a
worker name added after the address.

Stop it with Ctrl+C. It shuts down cleanly.

To keep it running after you close the terminal, start it inside tmux or
screen, or in the background with its output in a file, noting its process
number in miner.pid:

    DAGCORE_TOKEN_FILE=$PWD/api-token \
    DAGCORE_OVERRIDES_FILE=$PWD/overrides.env \
        nohup ./dagcore-miner > miner.log 2>&1 &
    echo $! > miner.pid
    tail -f miner.log

Stop it then, from this folder, with:

    kill $(cat miner.pid)

or, from the same terminal it was started in, with:  kill %1

Either one stops only this miner, cleanly, like Ctrl+C. Do not use
"pkill dagcore-miner" or "killall": run as root, they also stop the
installer's service, if the machine has one.


5. Open the dashboard
---------------------

In a browser on the same machine:

    http://localhost:8881/

If the log says "Metrics bind failed on 127.0.0.1:8881", another program
already uses that port - often the miner's own service, installed on the
same machine. This miner keeps mining, without a dashboard. Give it another
port with METRICS_PORT=8882 in config.env (or --metrics-port 8882) and open
http://localhost:8882/ instead.

It shows the hashrate, the shares the pool accepted and rejected, the card's
temperature and power, a 30-minute chart and the settings. /help on the same
address explains every number and control.

Changing anything from the dashboard needs the control token, which the miner
creates on its first start and prints in the log. It is the file api-token in
this folder (when started with the variables above):

    cat api-token

Paste it once under Settings -> Control token and press Save; the browser
keeps it.

From another computer. By default the dashboard answers only on this
machine. The safe way to reach it from elsewhere is an SSH tunnel; then open
the same address on that computer:

    ssh -L 8881:127.0.0.1:8881 user@this-rig

METRICS_BIND=0.0.0.0 in config.env opens it to the whole local network
instead, but then everyone on that network sees the page, including your
full wallet address.


6. More than one graphics card
------------------------------

By default the miner uses only the first card (GPU_DEVICE=0). Hashrate adds
up across cards, so on a rig with several, use all of them:

    ./dagcore-miner --gpu-device all

or set GPU_DEVICE=all in config.env. To choose cards, list their numbers:

    ./dagcore-miner --gpu-device 0,1

The numbers are the order in which the miner names the cards on start
("GPU 0: ...", "GPU 1: ..."), normally also the order "clinfo -l" shows them
in. GPU_INTENSITY takes one value for all cards or one per card, in the same
order: GPU_INTENSITY=80,60. With more than one card the dashboard's tuning
controls are off; see section 7.

--gpu-device counts the cards of one OpenCL platform, and every maker's
driver is a platform of its own. On a machine with, say, Intel graphics next
to an NVIDIA card, "--gpu-platform 1" picks the second platform if the first
line the miner prints names the wrong device.


7. Tuning (NVIDIA)
------------------

The dashboard can set an NVIDIA card's power limit, clock locks and clock
offsets, through nvidia-smi and the NVIDIA driver. The driver accepts those
changes only from root: started as a normal user, the miner shows the
controls, but applying one fails with the driver's refusal. Mining is
unaffected either way. A rig you want to tune is best set up with the
installer below, whose service runs as root.

The tuning controls work on a rig with one card only. With more than one,
the dashboard turns them off and says why: the miner cannot yet tell which
card nvidia-smi means by each number, and a power limit on the wrong card
is worse than none.

On AMD and Intel cards these controls are not available.


8. Running as a service
-----------------------

For a rig that mines from boot and restarts the miner by itself, use the
installer from the repository. It checks the machine, asks for the wallet,
builds the miner and sets up a systemd service:

    git clone https://github.com/dagcore/dagcore-miner.git
    cd dagcore-miner
    sudo ./install.sh

./install.sh --dry-run shows what it would do without changing anything. The
installer looks for an NVIDIA card and stops if it finds none; on other
hardware, run the miner from this folder as above.

With the service:

    sudo systemctl status dagcore-miner      # is it running?
    sudo journalctl -u dagcore-miner -f      # its log, live
    sudo systemctl restart dagcore-miner
    sudo cat /etc/dagcore-miner/api-token    # the dashboard's control token

Its settings are in /etc/dagcore-miner/config.env.


9. Is it paying?
----------------

Search your wallet address on the pool dashboard,
https://dagcore.net/mining.html, for your hashrate as the pool counts it,
your balance and every payout with its transaction. The pool takes the bare
address as the login: do not add a worker name (address.worker is refused).


Help, and the rest
------------------

    Download page   https://dagcore.net/download.html
    Quick questions https://t.me/dagcorenet
    Source code     https://github.com/dagcore/dagcore-miner

DagCore Miner is free software under the MIT licence (the LICENSE file in
this folder). It is derived from DagTech Miner by Dawie Nel / DagTech Ltd, who
remain the copyright holders of the original work. The dashboard uses the IBM
Plex Mono typeface under the SIL Open Font License 1.1 (dashboard/OFL.txt).
