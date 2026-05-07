#!/bin/bash
# stop.sh — Detiene todos los procesos PIBL y TWS
echo "[*] Deteniendo PIBL y TWS..."

if [ -f .pids ]; then
    read -r PIDS < .pids
    for pid in $PIDS; do
        kill "$pid" 2>/dev/null && echo "  [OK] Proceso $pid detenido"
    done
    rm -f .pids
else
    pkill -f "./bin/tws"  && echo "  [OK] TWS detenido"
    pkill -f "./bin/pibl" && echo "  [OK] PIBL detenido"
fi

echo "[*] Listo."
