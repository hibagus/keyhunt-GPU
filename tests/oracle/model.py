"""Independent, variable-time affine test mathematics; never production crypto."""
P = 0xFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFEFFFFFC2F
N = 0xFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFEBAAEDCE6AF48A03BBFD25E8CD0364141
G = (0x79BE667EF9DCBBAC55A06295CE870B07029BFCDB2DCE28D959F2815B16F81798,
     0x483ADA7726A3C4655DA4FBFC0E1108A8FD17B448A68554199C47D08FFB10D4B8)


def add(left, right):
    if left is None:
        return right
    if right is None:
        return left
    x, y = left
    u, v = right
    if x == u:
        if (y+v) % P == 0:
            return None
        slope = 3*x*x * pow(2*y, -1, P) % P
    else:
        slope = (v-y) * pow(u-x, -1, P) % P
    rx = (slope*slope-x-u) % P
    return rx, (slope*(x-rx)-y) % P


def multiply(scalar, point=G):
    result = None
    for bit in bin(scalar)[2:]:
        result = add(result, result)
        if bit == '1':
            result = add(result, point)
    return result


def encode(point, compressed=False):
    if point is None:
        return 'inf'
    x, y = point
    return f'{2+(y&1):02x}{x:064x}' if compressed else f'04{x:064x}{y:064x}'
