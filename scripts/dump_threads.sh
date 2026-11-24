#!/usr/bin/env bash
# Pede a cada thread do jogo em execucao que despeje seu estado no log
# (handler SIGUSR1 em src/main.c). Nao encerra o jogo.
#   scripts/dump_threads.sh            # todas as threads
#   scripts/dump_threads.sh --busy     # so as que estao usando CPU
PID="$(pgrep -n -x dw4_recomp)" || { echo "dw4_recomp nao esta rodando"; exit 1; }
if [ "${1:-}" = "--busy" ]; then
    TIDS=$(top -H -b -n1 -p "$PID" | awk 'NR>7 && $9+0 > 20 {print $1}')
else
    TIDS=$(ls /proc/"$PID"/task)
fi
for t in $TIDS; do
    python3 -c "import ctypes,signal; ctypes.CDLL(None).syscall(234,$PID,$t,signal.SIGUSR1)"
    sleep 0.05
done
echo "despejo pedido a: $(echo $TIDS | wc -w) thread(s) do PID $PID -- veja [DUMP] no log"
