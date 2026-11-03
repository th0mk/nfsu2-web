set -e
cd /tmp && rm -rf hbp hbp.tgz
for i in 1 2 3; do curl -fsSL -m 60 -o hbp.tgz https://github.com/dragonflylee/hacBrewPack/archive/refs/heads/master.tar.gz && break || sleep 3; done
mkdir hbp && tar xzf hbp.tgz -C hbp --strip-components=1
cd hbp && cp config.mk.template config.mk 2>/dev/null || true
make -j4 2>&1 | tail -3
ls -la hacbrewpack && cp hacbrewpack /out/hacbrewpack
