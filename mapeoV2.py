import socket
import time
import sys
from datetime import datetime
from pynput.keyboard import Controller

# =======================
# CONFIGURACIÓN
# =======================
UDP_PORT = 42101                 # Debe coincidir con el ESP32
HOLD_TIMEOUT_MS = 600          # Si no llega otro "1" dentro de este lapso, se suelta W
PRINT_RAW = False                 # Imprimir mensajes no parseables

# Entrada: mono-canal con tolerancia a paquetes de 1 o 2 valores
USE_TWO_VALUE_PACKETS_IF_PRESENT = True   # Si llegan "0 1" o "1 0", elegir un canal
USE_CHANNEL_INDEX = 1                     # 1 o 2 -> cuál canal usar si llegan dos valores
INVERT_CHANNELS = False                   # Si True y llegan 2 valores, intercambia CH1<->CH2
ACTIVE_HIGH = True                        # Si tu sensor envía 1 = evento; pon False si es activo-bajo

# Teclas
KEY_FORWARD = 'w'               # W
DISABLE_S = True                # Desactivar por completo la tecla S

# =======================
# ESTADO INTERNO
# =======================
kb = Controller()
forward_pressed = False
last_one_ts = 0.0

# =======================
# RED / SOCKET
# =======================
sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
sock.bind(("", UDP_PORT))
sock.settimeout(0.5)

def now_ms() -> float:
    return time.time() * 1000.0

def press_forward():
    global forward_pressed
    if not forward_pressed:
        kb.press(KEY_FORWARD)
        forward_pressed = True

def release_forward():
    global forward_pressed
    if forward_pressed:
        kb.release(KEY_FORWARD)
        forward_pressed = False

def release_all():
    try:
        release_forward()
    except Exception:
        pass

def parse_packet(msg: str):
    """
    Admite:
      - "0" o "1" (un solo canal)
      - "0 1", "1 0" (dos canales) -> devolver como tupla (ch1_bool, ch2_bool)
    Retorna:
      - ('single', bool)  si es un valor
      - ('dual', (bool,bool)) si son dos
      - None si no parsea
    """
    parts = msg.split()
    # Caso 1: un solo valor
    if len(parts) == 1 and parts[0].isdigit():
        v = int(parts[0]) != 0
        return ('single', v)
    # Caso 2: dos valores
    if len(parts) >= 2 and parts[0].isdigit() and parts[1].isdigit():
        ch1 = int(parts[0]) != 0
        ch2 = int(parts[1]) != 0
        return ('dual', (ch1, ch2))
    return None

def pick_active_bit(parsed):
    """
    A partir del parseo, elegir un único bit booleano de "evento".
    - single: usa ese valor
    - dual:   usa canal seleccionado (con posible inversión de canales)
    Aplica ACTIVE_HIGH al final.
    """
    kind, data = parsed
    if kind == 'single':
        val = data
    else:
        ch1, ch2 = data
        if INVERT_CHANNELS:
            ch1, ch2 = ch2, ch1
        if USE_CHANNEL_INDEX == 1:
            val = ch1
        else:
            val = ch2
    # Ajuste de polaridad
    return val if ACTIVE_HIGH else (not val)

print(f"Escuchando UDP en 0.0.0.0:{UDP_PORT} ...  "
      f"(mono-canal, hold_timeout={HOLD_TIMEOUT_MS} ms, use_two={USE_TWO_VALUE_PACKETS_IF_PRESENT}, "
      f"use_channel={USE_CHANNEL_INDEX}, invert_channels={INVERT_CHANNELS}, active_high={ACTIVE_HIGH})")

try:
    while True:
        try:
            data, addr = sock.recvfrom(1024)
        except socket.timeout:
            data = None

        if data:
            msg = data.decode("utf-8", errors="ignore").strip()
            ts = datetime.now().strftime("%H:%M:%S.%f")[:-3]
            parsed = parse_packet(msg)

            if parsed is None:
                if PRINT_RAW:
                    print(f"[{ts}] {addr[0]}  RAW: {msg}")
            else:
                # Seleccionar bit activo
                if parsed[0] == 'single':
                    event_bit = pick_active_bit(parsed)
                    print(f"[{ts}] {addr[0]}  VAL={1 if event_bit else 0}")
                else:
                    # dual
                    ch1, ch2 = parsed[1]
                    # Solo para log:
                    ch1_log, ch2_log = (ch1, ch2)
                    if INVERT_CHANNELS:
                        ch1_log, ch2_log = (ch2, ch1)
                    event_bit = pick_active_bit(parsed)
                    print(f"[{ts}] {addr[0]}  CH1={1 if ch1_log else 0}  CH2={1 if ch2_log else 0}  -> USING={USE_CHANNEL_INDEX} => {1 if event_bit else 0}")

                # Lógica mono-canal:
                # Si recibimos 1: renovar hold y asegurar W presionada.
                if event_bit:
                    last_one_ts = now_ms()
                    press_forward()
                # Si es 0, no hacemos nada inmediato; el timeout decide cuándo soltar.

        # Timeouts: soltar W si no hubo otro 1 dentro de HOLD_TIMEOUT_MS
        t = now_ms()
        if forward_pressed and (t - last_one_ts) > HOLD_TIMEOUT_MS:
            release_forward()

        # Evitar usar demasiada CPU
        time.sleep(0.001)

except KeyboardInterrupt:
    print("\nSaliendo por Ctrl+C, liberando teclas...")
except Exception as e:
    print(f"\nError: {e}", file=sys.stderr)
finally:
    release_all()
    sock.close()
    print("Listo. Teclas liberadas y socket cerrado.")
