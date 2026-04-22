import os
from pathlib import Path

p1 = Path.home() / ".wine/drive_c/Program Files/Common Files/VST3"
print(f"Path exists: {p1.exists()}")
if p1.exists():
    print(f"Contents: {os.listdir(p1)}")

# Try with escaped spaces just in case
p2 = Path("/home/dan/.wine/drive_c/Program Files/Common Files/VST3")
print(f"Alt Path exists: {p2.exists()}")
