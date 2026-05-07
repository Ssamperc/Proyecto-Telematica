#!/bin/bash
# start.sh — Arranca los 3 backends TWS + el PIBL
# Uso: ./start.sh [cache_ttl_segundos]
#      Por defecto TTL = 60 segundos

TTL=${1:-60}
WWW="./www"
LOG_DIR="./logs"
BIN="./bin"

mkdir -p "$LOG_DIR" cache

echo "========================================"
echo "  PIBL-WS — Sistema de inicio"
echo "  Cache TTL: ${TTL}s"
echo "========================================"

# Compilar si no existe
if [ ! -f "$BIN/tws" ] || [ ! -f "$BIN/pibl" ]; then
    echo "[*] Compilando..."
    make all
    if [ $? -ne 0 ]; then
        echo "[ERROR] Falló la compilación."
        exit 1
    fi
fi

# Matar procesos anteriores
echo "[*] Deteniendo instancias previas..."
pkill -f "$BIN/tws"  2>/dev/null
pkill -f "$BIN/pibl" 2>/dev/null
sleep 1

# Iniciar 3 servidores TWS
echo "[*] Iniciando TWS en puertos 9001, 9002, 9003..."
$BIN/tws 9001 "$LOG_DIR/tws_9001.log" "$WWW" &
TWS1_PID=$!
$BIN/tws 9002 "$LOG_DIR/tws_9002.log" "$WWW" &
TWS2_PID=$!
$BIN/tws 9003 "$LOG_DIR/tws_9003.log" "$WWW" &
TWS3_PID=$!

sleep 1

# Verificar que arrancaron
for port in 9001 9002 9003; do
    if ss -tlnp | grep -q ":$port "; then
        echo "  [OK] TWS escuchando en :$port"
    else
        echo "  [WARN] TWS en :$port no responde"
    fi
done

# Iniciar PIBL
echo "[*] Iniciando PIBL en puerto 8080 (TTL=${TTL}s)..."
$BIN/pibl config/pibl.conf "$TTL" &
PIBL_PID=$!

sleep 1

if ss -tlnp | grep -q ":8080 "; then
    echo "  [OK] PIBL escuchando en :8080"
else
    echo "  [ERROR] PIBL no arrancó"
fi

echo ""
echo "========================================"
echo "  PIDs: TWS=[$TWS1_PID,$TWS2_PID,$TWS3_PID] PIBL=[$PIBL_PID]"
echo "  Prueba: curl http://localhost:8080/"
echo "  Stop:   ./stop.sh"
echo "========================================"

# Guardar PIDs
echo "$TWS1_PID $TWS2_PID $TWS3_PID $PIBL_PID" > .pids
