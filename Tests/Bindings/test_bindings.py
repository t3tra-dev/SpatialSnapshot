#!/usr/bin/env python3
"""Independent BMFF fixture encoder and binding conformance tests (no C writer).

Synthetic encoded media exercises the container layer without a decoder. Actual codec
interoperability is checked separately with ImageIO/libheif/FFmpeg fixtures.
"""
from pathlib import Path
import copy
import json
import struct
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "Tests/Conformance"))
import test_conformance as ss


def u16(n): return struct.pack(">H", n)
def u32(n): return struct.pack(">I", n & 0xffffffff)
def u64(n): return struct.pack(">Q", n)
def box(t, p=b"", extended=False):
    t = t.encode() if isinstance(t, str) else u32(t)
    return (u32(1) + t + u64(len(p) + 16) if extended else u32(len(p) + 8) + t) + p
def full(t, p=b"", version=0, flags=0): return box(t, u32(version << 24 | flags) + p)
def matrix(): return b"".join(u32(v) for v in [65536, 0, 0, 0, 65536, 0, 0, 0, 1073741824])
def handler(t): return full("hdlr", bytes(4) + t.encode() + bytes(13))
def mdhd(d, version=1, scale=1_000_000_000):
    return full("mdhd", bytes(16 if version else 8) + u32(scale) + (u64(d) if version else u32(d)) + bytes(4), version)
def mvhd(d, version=1, scale=1_000_000_000):
    return full("mvhd", bytes(16 if version else 8) + u32(scale) + (u64(d) if version else u32(d)) +
                u32(65536) + u16(256) + bytes(10) + matrix() + bytes(24) + u32(3), version)
def tkhd(id, d, w=0, h=0, version=1, flags=7, mat=None, layer=0, volume=0):
    return full("tkhd", bytes(16 if version else 8) + u32(id) + bytes(4) + (u64(d) if version else u32(d)) +
                bytes(8) + u16(layer) + u16(0) + u16(volume) + bytes(2) + (mat or matrix()) + u32(w << 16) + u32(h << 16), version, flags)
def visual(w=4, h=3, extras=b""):
    return box("raw ", bytes(6) + u16(1) + bytes(16) + u16(w) + u16(h) + u32(72 << 16) * 2 +
               bytes(4) + u16(1) + bytes(32) + u16(24) + u16(65535) + extras)
def dinf(): return box("dinf", full("dref", u32(1) + full("url ", flags=1)))
def mebx(key=72, key_name="org.spatialsnapshot.ssps.packet-bundle", datatype="com.apple.metadata.datatype.raw-data", extras=b"", key_extras=b""):
    return box("mebx", bytes(6) + u16(1) + box("keys", box(key, box("keyd", b"mdta" + key_name.encode()) +
               box("dtyp", u32(1) + datatype.encode()) + key_extras)) + extras)
def stbl(entry, sizes, offsets, durations, composition=None, compact=None, co64=True, extras=b""):
    count = len(sizes)
    sizebox = full("stsz", u32(0) + u32(count) + b"".join(u32(s) for s in sizes))
    if compact:
        packed = bytes((sizes[i] << 4 | (sizes[i+1] if i+1 < count else 0)) for i in range(0, count, 2)) if compact == 4 else b"".join((bytes([s]) if compact == 8 else u16(s)) for s in sizes)
        sizebox = full("stz2", bytes(3) + bytes([compact]) + u32(count) + packed)
    result = full("stsd", u32(1) + entry) + full("stts", u32(len(durations)) + b"".join(u32(1) + u32(d) for d in durations))
    if composition is not None:
        result += full("ctts", u32(len(composition)) + b"".join(u32(1) + u32(d) for d in composition), 1)
    result += full("stsc", u32(1) + u32(1) * 3) + sizebox
    result += full("co64" if co64 else "stco", u32(count) + b"".join(u64(o) if co64 else u32(o) for o in offsets))
    return box("stbl", result + extras)
def bundles(data):
    header_size = struct.unpack_from("<H", data, 12)[0]
    result = []; start = 0; pos = header_size; previous = 0
    while pos < len(data):
        hs, typ = struct.unpack_from("<HH", data, pos + 4)
        timestamp = struct.unpack_from("<Q", data, pos + 20)[0]
        size = struct.unpack_from("<I", data, pos + 28)[0]
        if timestamp != previous and typ != 255:
            result.append(data[start:pos]); start = pos; previous = timestamp
        pos += hs + size
    result.append(data[start:]); return result

def atoms(data, start=0, end=None):
    end = len(data) if end is None else end
    while start < end:
        size, typ = struct.unpack_from(">I4s", data, start)
        head = 8
        if size == 1: size = struct.unpack_from(">Q", data, start+8)[0]; head = 16
        if not size: size = end-start
        yield typ.decode("latin1"), start+head, start+size
        prefix = {b"moov":0,b"trak":0,b"mdia":0,b"minf":0,b"stbl":0,b"tref":0,b"dinf":0,b"gmhd":0,
                  b"meta":4,b"iprp":0,b"ipco":0,b"stsd":8,b"dref":8,b"mebx":8,b"raw ":78,b"iinf":8}.get(typ)
        if prefix is not None: yield from atoms(data, start+head+prefix, start+size)
        start += size

def patch(data, typ, relative, fmt, value, occurrence=0):
    result = bytearray(data)
    where = [p for t,p,e in atoms(data) if t == typ][occurrence]
    struct.pack_into(">"+fmt, result, where+relative, value)
    return bytes(result)


def quicktime(*, data=None, key=72, video_order=(0, 1, 2), interval=1_000_000_000, version=1,
              width=4, height=3, mentry=None, ventry=None, metadata_extra=b"", video_extra=b"",
              metadata_flags=7, video_flags=7, meta_layer=0, meta_volume=0, gmhd=None,
              cdsc=None, brand=b"qt  ", compatible=b"qt  ", movie_scale=1_000_000_000,
              video_scale=1_000_000_000, meta_scale=1_000_000_000, extra_top=b"",
              include_metadata=True, duplicate_metadata=False, bundle_list=None,
              metadata_durations=None, video_durations=None, compact=None, co64=True,
              video_track_extra=b"", metadata_track_extra=b"", mat=None, d=None, prefix=b"", extended_ftyp=False):
    data = data or ss.stream(ss.VIDEO, kind=2)
    parts = bundle_list if bundle_list is not None else bundles(data)
    D = len(video_order) * interval if d is None else d
    ftyp = prefix + box("ftyp", brand + bytes(4) + compatible, extended=extended_ftyp)
    video = [bytes([i + 1]) * (width * height * 3) for i in video_order]
    metadata = [box(key, part) for part in parts]
    offsets = []; position = len(ftyp) + 8
    for sample in video + metadata: offsets.append(position); position += len(sample)
    mdat = box("mdat", b"".join(video + metadata))
    comp = [(i - j) * interval for j, i in enumerate(video_order)] if list(video_order) != sorted(video_order) else None
    vt = box("trak", tkhd(1, D, width, height, version, video_flags, mat=mat) + video_track_extra + box("mdia", mdhd(D, version, video_scale) + handler("vide") +
             box("minf", full("vmhd", bytes(8), flags=1) + dinf() + stbl(ventry or visual(width, height), list(map(len, video)), offsets[:len(video)],
                 video_durations or [interval] * len(video), comp, compact=compact, co64=co64, extras=video_extra))))
    mt = box("trak", tkhd(2, D, version=version, flags=metadata_flags, layer=meta_layer, volume=meta_volume) + metadata_track_extra +
             box("tref", box("cdsc", u32(1)) if cdsc is None else cdsc) +
             box("mdia", mdhd(D, version, meta_scale) + handler("meta") + box("minf",
                 (box("gmhd", full("gmin", bytes(12))) if gmhd is None else gmhd) + dinf() +
                 stbl(mentry or mebx(key), list(map(len, metadata)), offsets[len(video):], metadata_durations or [interval] * len(metadata), extras=metadata_extra))))
    return ftyp + mdat + box("moov", mvhd(D, version, movie_scale) + vt + (mt if include_metadata else b"") + (mt if duplicate_metadata else b"")) + extra_top


def heif(*, data=None, width=4, height=3, primary=1, metadata=2, version=2,
         item_name="SpatialSnapshot", mime="application/vnd.spatialsnapshot.ssps", encoding="", protection=0, hidden=0,
         targets=None, properties=b"", associations=None, essential_unknown=False, method=0, external=0,
         extent_count=1, overlap=False, include_metadata=True, extra_items=(), extra_refs=(), grid=False,
         grid_tile_properties=b"", meta_first=False, extent_length=None, handler_type="pict", primary_type="jpeg"):
    data = data or ss.stream(ss.MESH)
    targets = [primary] if targets is None else targets
    primary_data = b"\xff\xd8\xff\xd9" if not grid else bytes([0, 0, 0, 0]) + u16(width) + u16(height)
    items = [(primary, "grid" if grid else primary_type, "Primary", "", "", 0, 0, primary_data),
             (metadata, "mime", item_name, mime, encoding, protection, hidden, data)] if include_metadata else [(primary, primary_type, "Primary", "", "", 0, 0, primary_data)]
    items += list(extra_items)
    tile_id = max(primary, metadata) + 1
    if grid: items.append((tile_id, "jpeg", "Tile", "", "", 0, 0, b"\xff\xd8\xff\xd9"))
    ftyp = box("ftyp", b"mif1" + bytes(4) + b"mif1")
    idwidth = 4 if version == 3 else 2
    packid = u32 if idwidth == 4 else u16
    iinf = full("iinf", u32(len(items)) + b"".join(full("infe", packid(id) + u16(protect) + typ.encode() + name.encode() + b"\0" +
                (ct.encode() + b"\0" + ce.encode() + b"\0" if typ == "mime" else b""), version, flags) for id, typ, name, ct, ce, protect, flags, payload in items), 1)
    refs = [(metadata, "cdsc", targets)] if include_metadata else []
    refs += list(extra_refs)
    if grid: refs.append((primary, "dimg", [tile_id]))
    iref = full("iref", b"".join(box(typ, packid(id) + u16(len(ts)) + b"".join(packid(t) for t in ts)) for id, typ, ts in refs), 1 if idwidth == 4 else 0)
    ispe = full("ispe", u32(width) + u32(height))
    props = ispe + properties
    if associations is None: associations = [1] + ([0x82] if essential_unknown else ([2] if properties else []))
    ipma = packid(primary) + bytes([len(associations)]) + bytes(associations)
    if grid:
        tile_assocs = [1]
        if grid_tile_properties: props += grid_tile_properties; tile_assocs.append(2 if not properties else 3)
        ipma += packid(tile_id) + bytes([len(tile_assocs)]) + bytes(tile_assocs)
    iprp = box("iprp", box("ipco", props) + full("ipma", u32(2 if grid else 1) + ipma, 1 if idwidth == 4 else 0))
    def make_meta(base):
        position = base; locations = []
        for id, typ, name, ct, ce, protect, flags, payload in items:
            m = method if id == metadata else 0
            dr = external if id == metadata else 0
            cnt = extent_count if id == metadata else 1
            off = base if id == metadata and overlap else position
            length = extent_length if id == metadata and extent_length is not None else len(payload)
            locations.append(u32(id) + u16(m) + u16(dr) + u16(cnt) + b"".join(u64(off) + u64(length) for _ in range(cnt)))
            position += len(payload)
        iloc = full("iloc", b"\x88\0" + u32(len(items)) + b"".join(locations), 2)
        return full("meta", handler(handler_type) + full("pitm", packid(primary), 1 if idwidth == 4 else 0) + iinf + iloc + iref + iprp)
    media = b"".join(it[-1] for it in items)
    if meta_first:
        meta = make_meta(0); return ftyp + make_meta(len(ftyp) + len(meta) + 8) + box("mdat", media)
    return ftyp + box("mdat", media) + make_meta(len(ftyp) + 8)


def cases():
    result = {}
    def add(name, data, status="MALFORMED", domains=("BINDING_MALFORMED",)):
        result[name] = (data, status, domains)
    for name, kw in [("qt-minimal", {}), ("qt-bframes", {"video_order": (0, 2, 1)}), ("qt-v0", {"version": 0}),
                     ("qt-arbitrary-key", {"key": 0x12345678}), ("qt-uuid-key", {"key": 0x75756964}),
                     ("qt-stco-stz2", {"compact": 8, "co64": False}),
                     ("qt-unknown-key-child", {"mentry": mebx(key_extras=box("free", b"opaque"))}),
                     ("qt-unknown-entry-child", {"mentry": mebx(extras=box("free", b"opaque"))})]:
        add(name, quicktime(**kw), "OK", ())
    for name, kw in [("heif-minimal", {}), ("heif-meta-first", {"meta_first": True}),
                     ("heif-wide-ids", {"primary": 70000, "metadata": 99000, "version": 3}),
                     ("heif-grid", {"grid": True}), ("heif-square-pasp", {"properties": box("pasp", u32(2) * 2)}),
                     ("heif-unknown-property", {"properties": box("zzzz", b"opaque")}),
                     ("heif-identity-clap", {"properties": box("clap", u32(4)+u32(1)+u32(3)+u32(1)+u32(0)+u32(1)+u32(0)+u32(1))})]:
        add(name, heif(**kw), "OK", ())
    for name, kw in [("qt-missing", {"include_metadata": False}), ("qt-reserved-zero", {"key": 0}),
                     ("qt-reserved-future", {"key": 0xffffffff}), ("qt-key-nul", {"mentry": mebx(key_name="org.spatialsnapshot.ssps.packet-bundle\0")}),
                     ("qt-wrong-datatype", {"mentry": mebx(datatype="raw-data")}),
                     ("qt-locale", {"mentry": mebx(key_extras=box("loca", bytes(4)))}),
                     ("qt-btrt", {"mentry": mebx(extras=box("btrt", bytes(12)))}),
                     ("qt-no-cdsc", {"cdsc": b""}), ("qt-two-cdsc", {"cdsc": box("cdsc", u32(1)*2)}),
                     ("qt-meta-ctts", {"metadata_extra": full("ctts", u32(1) + u32(3) + u32(0))}),
                     ("qt-meta-stss", {"metadata_extra": full("stss", u32(1)+u32(1))}),
                     ("qt-meta-flags", {"metadata_flags": 3}), ("qt-video-flags", {"video_flags": 6}),
                     ("qt-meta-layer", {"meta_layer": 1}), ("qt-meta-volume", {"meta_volume": 256}),
                     ("qt-gmin", {"gmhd": box("gmhd", full("gmin", bytes(11)+b"\1"))}),
                     ("qt-movie-timescale", {"movie_scale": 600}), ("qt-video-timescale", {"video_scale": 600}),
                     ("qt-meta-timescale", {"meta_scale": 600}), ("qt-major-brand", {"brand": b"isom"}),
                     ("qt-compatible-brand", {"compatible": b"isom"}), ("qt-moof", {"extra_top": box("moof")}),
                     ("qt-clap", {"ventry": visual(extras=box("clap", bytes(32)))}),
                     ("qt-pasp", {"ventry": visual(extras=box("pasp", u32(2)+u32(1)))}),
                     ("qt-edts", {"video_track_extra": box("edts", full("elst", u32(0)))}),
                     ("qt-clip", {"video_track_extra": box("clip")}),
                     ("qt-raster", {"width": 8}), ("qt-interval", {"metadata_durations": [1,1,1]}),
                     ("qt-last-video-duration", {"video_durations": [1_000_000_000]*2+[999999999]})]:
        add(name, quicktime(**kw))
    for name, kw in [("heif-missing", {"include_metadata": False}), ("heif-name", {"item_name": "spatialsnapshot"}),
                     ("heif-mime-parameter", {"mime": "application/vnd.spatialsnapshot.ssps;v=1"}),
                     ("heif-encoding", {"encoding": "gzip"}), ("heif-hidden", {"hidden": 1}),
                     ("heif-protected", {"protection": 1}), ("heif-targets", {"targets": [1,1]}),
                     ("heif-no-target", {"targets": []}), ("heif-external", {"external": 1}),
                     ("heif-extents", {"extent_count": 2}), ("heif-overlap", {"overlap": True}),
                     ("heif-raster", {"width": 8}), ("heif-rotate", {"properties": box("irot", b"\0")}),
                     ("heif-mirror", {"properties": box("imir", b"\0")}),
                     ("heif-pasp", {"properties": box("pasp", u32(2)+u32(1))}),
                     ("heif-grid-transform", {"grid": True, "grid_tile_properties": box("irot", b"\1")}),
                     ("heif-handler", {"handler_type": "meta"}), ("heif-video", {"data": ss.stream(ss.VIDEO, kind=2)}),
                     ("heif-derived", {"primary_type": "iden"})]:
        add(name, heif(**kw))
    add("heif-essential-unknown", heif(properties=box("zzzz", b"opaque"), essential_unknown=True), "UNSUPPORTED", ("UNSUPPORTED_BINDING",))
    add("heif-major", heif(data=ss.stream(ss.MESH, major=2)), "UNSUPPORTED", ("UNSUPPORTED_SSPS",))
    add("qt-major", quicktime(data=ss.stream(ss.VIDEO, kind=2, major=2)), "UNSUPPORTED", ("UNSUPPORTED_SSPS",))
    corrupt = bytearray(ss.stream(ss.MESH)); corrupt[45] ^= 1
    add("heif-crc", heif(data=bytes(corrupt)), "MALFORMED", ("SSPS_MALFORMED",))
    corrupt = bytearray(ss.stream(ss.VIDEO, kind=2)); corrupt[45] ^= 1
    add("qt-crc", quicktime(data=bytes(corrupt)), "MALFORMED", ("SSPS_MALFORMED",))
    # Explicit IFD construction keeps byte order and count widths independent.
    def exif(value, le=True):
        f = "<" if le else ">"
        return bytes(4)+(b"II" if le else b"MM")+struct.pack(f+"HIH",42,8,1)+struct.pack(f+"HHI",0x112,3,1)+struct.pack(f+"H",value)+bytes(6)
    for orientation in [1,2,8]:
        for le in [False, True]:
            add(f"heif-exif-{orientation}-{le}", heif(extra_items=[(3,"Exif","Exif","","",0,0,exif(orientation,le))], extra_refs=[(3,"cdsc",[1])]),
                "OK" if orientation == 1 else "MALFORMED", () if orientation == 1 else ("BINDING_MALFORMED",))
    for orientation in [1, 6]:
        for form in ["attribute", "element", "qualified", "utf16", "entity"]:
            val = f"&#{48+orientation};" if form == "entity" else str(orientation)
            body = f'<x:Description xmlns:z="http://ns.adobe.com/tiff/1.0/" z:Orientation="{val}"/>' if form in ["attribute","utf16","entity"] else f'<z:Orientation xmlns:z="http://ns.adobe.com/tiff/1.0/">{val}</z:Orientation>'
            if form == "qualified": body = f'<z:Orientation xmlns:z="http://ns.adobe.com/tiff/1.0/"><x:value>{val}</x:value></z:Orientation>'
            xml = f'<x:RDF xmlns:x="http://www.w3.org/1999/02/22-rdf-syntax-ns#">{body}</x:RDF>'.encode("utf-16" if form == "utf16" else "utf-8")
            add(f"heif-xmp-{form}-{orientation}", heif(extra_items=[(3,"mime","XMP","application/rdf+xml","",0,0,xml)], extra_refs=[(3,"cdsc",[1])]),
                "OK" if orientation == 1 else "MALFORMED", () if orientation == 1 else ("BINDING_MALFORMED",))
    add("heif-independent-errors", heif(properties=box("irot", b"\1"), width=8, hidden=1))
    add("qt-independent-errors", quicktime(video_flags=0, metadata_flags=0, meta_layer=1, meta_volume=256))
    qt = quicktime()
    host = ("QUICKTIME_MALFORMED",)
    for typ, rel, value, which in [("stsc",8,0,0),("stsc",12,0,0),("stsc",16,2,0),
        ("stsz",8,4,0),("stts",8,2,0),("co64",8,0,0),("co64",8,2**64-1,0),
        ("tkhd",20,1,1),("tkhd",20,0,0),("mdhd",20,0,0)]:
        add(f"qt-host-{typ}-{rel}-{value}-{which}",patch(qt,typ,rel,"Q" if typ=="co64" else "I",value,which),domains=host)
    for typ, rel, value, which in [("tkhd",52,0,0),("tkhd",88,0,0),("tkhd",88,65536,1),
        ("tkhd",46,1,1),("mdhd",24,3000000001,0),("mdhd",24,3000000001,1),("mvhd",24,3000000001,0),
        ("tkhd",28,3000000001,0),("tkhd",28,3000000001,1)]:
        add(f"qt-binding-{typ}-{rel}-{which}",patch(qt,typ,rel,"Q" if rel in [24,28] else "H" if rel==46 else "I",value,which))
    for value in [0,1,7,9,0xffffffff]:
        altered=bytearray(qt); sample_offset = next(p for t,p,e in atoms(qt) if t=="mdat")+108
        struct.pack_into(">I",altered,sample_offset,value)
        add(f"qt-value-size-{value}",bytes(altered))
    add("qt-value-type",patch(qt,"mdat",108+4,"I",99))
    parts=bundles(ss.stream(ss.VIDEO,kind=2))
    add("qt-split-packet",quicktime(bundle_list=[parts[0]+parts[1][:50],parts[1][50:],parts[2]]))
    add("qt-wrong-bundle-timestamp",quicktime(bundle_list=[parts[0]+parts[1],b"",parts[2]]))
    add("qt-end-own-sample",quicktime(bundle_list=parts+[b""]))
    add("qt-ctts-without-reordering",quicktime(video_extra=full("ctts",u32(1)+u32(3)+u32(0))))
    cslg = lambda vals: full("cslg",b"".join(u64(v&0xffffffffffffffff) for v in vals),1)
    add("qt-cslg",quicktime(video_order=(0,2,1),video_extra=cslg([0,-1000000000,1000000000,0,3000000000])),"OK",())
    add("qt-interframe",quicktime(video_extra=full("stss",u32(1)+u32(1))),"OK",())
    add("qt-extended-prefix",quicktime(prefix=box("free",bytes(128),extended=True),extended_ftyp=True),"OK",())
    add("heif-forward-compatible",heif(data=ss.stream(ss.MESH[:-1]+[ss.Packet(0x8000,b"extension",flags=0),ss.MESH[-1]],minor=1,extra=b"opaque header")),"OK",())
    add("qt-forward-compatible",quicktime(data=ss.stream(ss.VIDEO[:-1]+[ss.Packet(0x8000,b"extension",flags=0,time=2_000_000_000),ss.VIDEO[-1]],kind=2,minor=1,extra=b"opaque header")),"OK",())
    add("qt-cslg-contradiction",quicktime(video_order=(0,2,1),video_extra=cslg([0,-1000000000,1000000000,0,3000000001])))
    add("qt-stz2-four-bit",quicktime(width=1,height=1,compact=4,data=ss.stream([ss.replace(p,raw=ss.camera(width=1,height=1,cx=0,cy=0)) if p.type==ss.CAMERA else p for p in ss.VIDEO],kind=2)),"OK",())
    add("qt-stz2-sixteen-bit",quicktime(compact=16),"OK",())
    add("qt-missing-avcC",quicktime(ventry=visual()[:4]+b"avc1"+visual()[8:]),domains=host)
    add("qt-duration-v1-long",quicktime(interval=2_000_000_000,data=ss.stream([ss.replace(p,time=p.time*2) for p in ss.VIDEO],kind=2)),"OK",())
    add("qt-count-resource",patch(qt,"stsz",8,"I",20000001),"RESOURCE_LIMIT",("RESOURCE_LIMIT",))
    add("qt-description-resource",patch(qt,"stsd",4,"I",257),"RESOURCE_LIMIT",("RESOURCE_LIMIT",))
    he = heif()
    for typ,rel,value in [("iloc",6,65537),("iinf",4,65537),("ipma",4,65537)]:
        add(f"heif-count-resource-{typ}",patch(he,typ,rel,"I",value),"RESOURCE_LIMIT",("RESOURCE_LIMIT",))
    add("heif-implicit-ssps-length",heif(meta_first=True,extent_length=0))
    add("heif-no-image-location",patch(he,"iloc",18,"H",0),domains=("HEIF_MALFORMED",))
    add("heif-duplicate-id",patch(he,"infe",4,"H",1,1),domains=("HEIF_MALFORMED",))
    add("heif-item-offset-overflow",patch(he,"iloc",20,"Q",2**64-1),domains=("HEIF_MALFORMED",))
    add("heif-unknown-infe-version",patch(he,"infe",0,"B",4),"UNSUPPORTED",("UNSUPPORTED_BINDING",))
    add("heif-invalid-ipma-index",patch(he,"ipma",11,"B",0xff),domains=("HEIF_MALFORMED",))
    for suffix,xml,expected in [
        ("utf16-surrogate",'<r xmlns:t="http://ns.adobe.com/tiff/1.0/" name="🎥" t:Orientation="1"/>'.encode("utf-16"),"OK"),
        ("utf32",'<r xmlns:t="http://ns.adobe.com/tiff/1.0/" t:Orientation="1"/>'.encode("utf-32"),"OK"),
        ("namespace-alias",b'<r xmlns:x="http://example.com/" x:Orientation="6"/>',"OK"),
        ("namespace-shadow",b'<r xmlns:x="http://ns.adobe.com/tiff/1.0/"><s xmlns:x="http://example.com/" x:Orientation="6"/></r>',"OK"),
        ("cdata",b'<r xmlns:x="http://ns.adobe.com/tiff/1.0/"><x:Orientation><![CDATA[1]]></x:Orientation></r>',"OK"),
        ("cdata-entity",b'<r xmlns:x="http://ns.adobe.com/tiff/1.0/"><x:Orientation><![CDATA[&#49;]]></x:Orientation></r>',"MALFORMED")]:
        add("heif-xmp-"+suffix,heif(extra_items=[(3,"mime","XMP","application/rdf+xml","",0,0,xml)],extra_refs=[(3,"cdsc",[1])]),expected,() if expected=="OK" else ("BINDING_MALFORMED",))
    for prefix, source in [("qt", quicktime()), ("heif", heif())]:
        for cut in [1,4,7,12,19,len(source)-1]:
            add(f"{prefix}-truncated-{cut}", source[:cut], "MALFORMED", ("QUICKTIME_MALFORMED" if prefix == "qt" else "HEIF_MALFORMED",))
    return result


def main():
    corpus = cases()
    if len(sys.argv) > 1 and sys.argv[1] == "--generate":
        path = ROOT / "Fixtures/bindings"
        path.mkdir(parents=True, exist_ok=True)
        manifest = {}
        for name, (data, status, domains) in corpus.items():
            suffix = ".mov" if name.startswith("qt-") else ".heif"
            (path/(name+suffix)).write_bytes(data)
            manifest[name+suffix] = {"status":status, "domains": domains}
        (path/"manifest.json").write_text(json.dumps(manifest, indent=2)+"\n")
        print(f"Generated {len(corpus)} binding fixtures"); return
    exe = sys.argv[1] if len(sys.argv) > 1 else str(ROOT/"build/ssvalidate")
    failures = []
    with tempfile.TemporaryDirectory() as tmp:
        for name, (data, status, domains) in corpus.items():
            file = Path(tmp)/name; file.write_bytes(data)
            kind = "--quicktime" if name.startswith("qt-") else "--heif"
            p = subprocess.run([exe, "--json", kind, str(file)], capture_output=True, text=True)
            try: info = json.loads(p.stdout)
            except Exception:
                failures.append((name, p.returncode, p.stdout, p.stderr)); continue
            observed = {d["domain"] for d in info.get("diagnostics", [])}
            if info["status"] != status or not set(domains) <= observed or ("independent-errors" in name and len(info.get("diagnostics", [])) < 3):
                failures.append((name, status, domains, info))
            if status == "OK":
                automatic = subprocess.run([exe,"--json",str(file)],capture_output=True,text=True)
                if automatic.returncode or json.loads(automatic.stdout)["status"] != "OK":
                    failures.append((name,"automatic container detection",automatic.stdout,automatic.stderr))
        # Exercise writer-only normalization with independently generated hosts.
        # The normative reader must still reject irot=0 on a bound file.
        binder = Path(exe).resolve().with_name("ss_binding_tests")
        if binder.exists():
            spatial = Path(tmp)/"source.ssps"; spatial.write_bytes(ss.stream(ss.MESH))
            for rotation in [0, 1, 4]:
                host = Path(tmp)/f"rotation-{rotation}.heif"
                host.write_bytes(heif(include_metadata=False, properties=box("irot", bytes([rotation])),
                                      associations=[1, 0x82]))
                destination = Path(tmp)/f"normalized-{rotation}.heif"
                p = subprocess.run([str(binder), "--bind", "heif", str(host), str(spatial), str(destination)],
                                   capture_output=True, text=True)
                if rotation == 0:
                    if p.returncode:
                        failures.append(("writer identity rotation", p.stdout, p.stderr))
                    else:
                        check = subprocess.run([exe, "--json", "--heif", str(destination)], capture_output=True, text=True)
                        if check.returncode:
                            failures.append(("normalized writer output", check.stdout, check.stderr))
                elif not p.returncode or destination.exists():
                    failures.append(("writer accepted nonidentity/reserved rotation", rotation))
            bound = Path(tmp)/"bound-identity.heif"
            bound.write_bytes(heif(properties=box("irot", b"\x00"), associations=[1, 0x82]))
            check = subprocess.run([exe, "--json", "--heif", str(bound)], capture_output=True, text=True)
            if json.loads(check.stdout)["status"] != "MALFORMED":
                failures.append(("reader accepted identity rotation on bound image", check.stdout))
            destination = Path(tmp)/"must-not-repair.heif"
            p = subprocess.run([str(binder), "--bind", "heif", str(bound), str(spatial), str(destination)],
                               capture_output=True, text=True)
            if not p.returncode or destination.exists():
                failures.append(("writer repaired an already bound malformed image", p.stdout, p.stderr))
    for failure in failures: print(failure)
    if failures: raise SystemExit(f"{len(failures)} / {len(corpus)} binding cases failed")
    print(f"{len(corpus)} independent binding cases passed")


if __name__ == "__main__": main()
