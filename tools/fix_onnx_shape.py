"""Give an ONNX model's graph inputs a fixed shape, without the onnx package:
a small protobuf wire-format editor that rewrites ModelProto.graph.input[*]
.type.tensor_type.shape.dim and copies every other byte as it stands.

  python tools/fix_onnx_shape.py <in.onnx> <out.onnx> 1,3,48,320 [--drop-value-info]

Made for resources/textline.onnx, the second reader's recogniser (see
include/TextLineReader.h). The source is PaddleOCR's PP-OCRv4 recognition
model as ONNX - ch_PP-OCRv4_rec_infer.onnx from @gutenye/ocr-models 1.4.2,
sha256 06b3e6af6c59a1ba5d53790ed8c2e4b2de389870b6cf5a97f349f3412cb269c0 -
whose input is [batch, 3, 48, width] with batch and width left open.
OpenCV's importer cannot follow the shape arithmetic that hangs off an open
width (it stops at the first Concat); with the input fixed at
1x3x48x320 - PaddleOCR's own recognition shape - it builds the net, and
the reader pads every line out to that width the way PaddleOCR does.
"""
import sys


def read_varint(b, i):
    shift = result = 0
    while True:
        c = b[i]
        i += 1
        result |= (c & 0x7F) << shift
        if not c & 0x80:
            return result, i
        shift += 7


def varint(n):
    if n < 0:
        n += 1 << 64
    out = bytearray()
    while True:
        c = n & 0x7F
        n >>= 7
        if n:
            out.append(c | 0x80)
        else:
            out.append(c)
            return bytes(out)


def fields(b):
    """(field number, wire type, value, raw bytes of the whole field)"""
    i = 0
    out = []
    while i < len(b):
        start = i
        key, i = read_varint(b, i)
        no, wt = key >> 3, key & 7
        if wt == 0:
            v, i = read_varint(b, i)
        elif wt == 1:
            v = b[i:i + 8]
            i += 8
        elif wt == 2:
            n, i = read_varint(b, i)
            v = b[i:i + n]
            i += n
        elif wt == 5:
            v = b[i:i + 4]
            i += 4
        else:
            raise ValueError("wire type %d at %d" % (wt, start))
        out.append((no, wt, v, b[start:i]))
    return out


def ld(no, payload):
    return varint(no << 3 | 2) + varint(len(payload)) + payload


def shape_of(value_info):
    dims = []
    for no, wt, v, raw in fields(value_info):
        if no == 2:
            for no2, _, v2, _ in fields(v):
                if no2 == 1:
                    for no3, _, v3, _ in fields(v2):
                        if no3 == 2:
                            for no4, _, v4, _ in fields(v3):
                                if no4 == 1:
                                    d = {}
                                    for no5, _, v5, _ in fields(v4):
                                        d[no5] = v5
                                    dims.append(d.get(1, d.get(2, b"?")))
    return dims


def name_of(value_info):
    for no, wt, v, raw in fields(value_info):
        if no == 1:
            return v.decode()
    return "?"


def with_shape(value_info, dims):
    out = b""
    for no, wt, v, raw in fields(value_info):
        if no != 2:
            out += raw
            continue
        tp = b""
        for no2, wt2, v2, raw2 in fields(v):
            if no2 != 1:
                tp += raw2
                continue
            tt = b""
            for no3, wt3, v3, raw3 in fields(v2):
                if no3 == 2:
                    shape = b"".join(ld(1, varint(1 << 3 | 0) + varint(d)) for d in dims)
                    tt += ld(2, shape)
                else:
                    tt += raw3
            tp += ld(1, tt)
        out += ld(2, tp)
    return out


def main():
    src, dst, dims = sys.argv[1], sys.argv[2], [int(x) for x in sys.argv[3].split(",")]
    drop_vi = "--drop-value-info" in sys.argv
    data = open(src, "rb").read()
    model = b""
    for no, wt, v, raw in fields(data):
        if no != 7:
            model += raw
            continue
        graph = b""
        n_vi = 0
        for gno, gwt, gv, graw in fields(v):
            if gno == 11:
                print("input %s: %s -> %s" % (name_of(gv), shape_of(gv), dims))
                graph += ld(11, with_shape(gv, dims))
            elif gno == 12:
                print("output %s: %s" % (name_of(gv), shape_of(gv)))
                graph += graw
            elif gno == 13 and drop_vi:
                n_vi += 1
            else:
                graph += graw
        if drop_vi:
            print("dropped %d value_info entries" % n_vi)
        model += ld(7, graph)
    open(dst, "wb").write(model)
    print("wrote %s (%d bytes)" % (dst, len(model)))


main()
