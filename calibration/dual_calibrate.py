#!/usr/bin/env python3
import time
import tkinter as tk
from tkinter import messagebox

from telemetrix import telemetrix

# === Settings (edit here) ===
COM_PORT = "/dev/ttyACM0"  # e.g. "/dev/ttyACM0", "COM3", "/dev/cu.usbmodem*"
SERVO_A_PIN = 9  # Servo A signal (D9)
SERVO_B_PIN = 10  # Servo B signal (D10)
INVERT_A = False  # True if A moves opposite
INVERT_B = False  # True if B moves opposite

board = None


def open_board():
    global board
    try:
        board = telemetrix.Telemetrix(com_port=COM_PORT)
        board.set_pin_mode_servo(SERVO_A_PIN)  # attach A
        board.set_pin_mode_servo(SERVO_B_PIN)  # attach B
        time.sleep(0.5)
        go_center_both()
        print(f"Connected to {COM_PORT} via Telemetrix (A:D{SERVO_A_PIN}, B:D{SERVO_B_PIN})")
    except Exception as e:
        messagebox.showerror("Connection Error", f"Could not open Telemetrix on {COM_PORT}:\n{e}")
        board = None


def _clamp(a):
    return max(0, min(180, int(round(float(a)))))


def _maybe_invert(angle, inv):
    return 180 - angle if inv else angle


def send_angle_A(angle_deg):
    if board is None:
        return
    out = _clamp(_maybe_invert(angle_deg, INVERT_A))
    board.servo_write(SERVO_A_PIN, out)
    print(f"A → {out}°")


def send_angle_B(angle_deg):
    if board is None:
        return
    out = _clamp(_maybe_invert(angle_deg, INVERT_B))
    board.servo_write(SERVO_B_PIN, out)
    print(f"B → {out}°")


def on_slider_A(val):
    try:
        send_angle_A(val)
    except ValueError:
        pass


def on_slider_B(val):
    try:
        send_angle_B(val)
    except ValueError:
        pass


def go_center_A():
    sliderA.set(90.0)
    send_angle_A(90.0)


def go_center_B():
    sliderB.set(90.0)
    send_angle_B(90.0)


def go_center_both():
    sliderA.set(90.0)
    sliderB.set(90.0)
    # back-to-back writes are effectively simultaneous
    send_angle_A(90.0)
    send_angle_B(90.0)


def nudge_A(delta):
    cur = float(sliderA.get())
    newv = _clamp(cur + delta)
    sliderA.set(newv)
    send_angle_A(newv)


def nudge_B(delta):
    cur = float(sliderB.get())
    newv = _clamp(cur + delta)
    sliderB.set(newv)
    send_angle_B(newv)


def on_close():
    global board
    try:
        if board is not None:
            board.shutdown()
            print("Telemetrix shutdown.")
    except Exception as e:
        print(f"Shutdown error: {e}")
    root.destroy()


# === Tkinter GUI ===
root = tk.Tk()
root.title("Dual SG90 Calibrate (Telemetrix D9 & D10)")
root.geometry("420x260")

# Servo A
frameA = tk.LabelFrame(root, text="Servo A (D9)")
frameA.pack(fill="x", padx=16, pady=(10, 6))

sliderA = tk.Scale(
    frameA,
    from_=0.0,
    to=180.0,
    resolution=1.0,
    orient="horizontal",
    command=on_slider_A,
    label="Angle A (deg)",
)
sliderA.set(90.0)
sliderA.pack(fill="x", padx=8, pady=(6, 4))

rowA = tk.Frame(frameA)
rowA.pack(fill="x", padx=8, pady=(0, 6))
tk.Button(rowA, text="◀ A -1°", command=lambda: nudge_A(-1), width=10).pack(side="left", padx=4)
tk.Button(rowA, text="A Center", command=go_center_A, width=10).pack(side="left", padx=4)
tk.Button(rowA, text="A +1° ▶", command=lambda: nudge_A(+1), width=10).pack(side="left", padx=4)

# Servo B
frameB = tk.LabelFrame(root, text="Servo B (D10)")
frameB.pack(fill="x", padx=16, pady=(6, 6))

sliderB = tk.Scale(
    frameB,
    from_=0.0,
    to=180.0,
    resolution=1.0,
    orient="horizontal",
    command=on_slider_B,
    label="Angle B (deg)",
)
sliderB.set(90.0)
sliderB.pack(fill="x", padx=8, pady=(6, 4))

rowB = tk.Frame(frameB)
rowB.pack(fill="x", padx=8, pady=(0, 6))
tk.Button(rowB, text="◀ B -1°", command=lambda: nudge_B(-1), width=10).pack(side="left", padx=4)
tk.Button(rowB, text="B Center", command=go_center_B, width=10).pack(side="left", padx=4)
tk.Button(rowB, text="B +1° ▶", command=lambda: nudge_B(+1), width=10).pack(side="left", padx=4)

# Both
rowBoth = tk.Frame(root)
rowBoth.pack(fill="x", padx=16, pady=(2, 8))
tk.Button(rowBoth, text="Center BOTH", command=go_center_both, width=12).pack(side="left", padx=4)

root.protocol("WM_DELETE_WINDOW", on_close)

open_board()
root.mainloop()
