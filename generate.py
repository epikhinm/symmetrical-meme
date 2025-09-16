import sys
import random

if __name__ == "__main__":
    if len(sys.argv) != 2:
        print("Usage: python generate.py <count>")
        sys.exit(1)
    count = int(sys.argv[1])
    # I specified seed to have reproducable files between tests
    r = random.seed(42)
    for el in range(count):
        c = random.randint(1, count)
        print(f"http://api.tech.com/item/{el} {c}")