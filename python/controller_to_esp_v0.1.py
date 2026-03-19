import time
import pygame
import argparse
from websocket import create_connection

DEFAULT_IP = "192.168.50.49"   # fallback if not provided

SEND_PERIOD_S = 0.05
DEADZONE = 0.08

def apply_deadzone(v, dz=DEADZONE):
    return 0.0 if abs(v) < dz else v

def parse_args():
    parser = argparse.ArgumentParser(description="Xbox controller → ESP32 WebSocket client")

    parser.add_argument(
        "--ip",
        type=str,
        default=DEFAULT_IP,
        help=f"ESP32 IP address (default: {DEFAULT_IP})"
    )

    return parser.parse_args()

def main():
    args = parse_args()
    ws_url = f"ws://{args.ip}:81/"

    pygame.init()
    pygame.joystick.init()

    if pygame.joystick.get_count() == 0:
        raise RuntimeError("No controller detected")

    joystick = pygame.joystick.Joystick(0)
    joystick.init()

    print(f"Controller: {joystick.get_name()}")
    print(f"Connecting to {ws_url}")

    ws = create_connection(ws_url, timeout=5)
    print("Connected")

    last_send = 0

    try:
        while True:
            pygame.event.pump()

            lx = apply_deadzone(joystick.get_axis(0))
            ly = apply_deadzone(joystick.get_axis(1))
            rx = apply_deadzone(joystick.get_axis(2))
            ry = apply_deadzone(joystick.get_axis(3))

            a = joystick.get_button(0)
            b = joystick.get_button(1)

            now = time.time()
            if now - last_send >= SEND_PERIOD_S:
                msg = f"{lx:.3f},{ly:.3f},{rx:.3f},{ry:.3f},{a},{b}"
                ws.send(msg)
                print(msg)
                last_send = now

            time.sleep(0.005)

    except KeyboardInterrupt:
        print("Stopping...")
    finally:
        ws.close()
        pygame.quit()

if __name__ == "__main__":
    main()