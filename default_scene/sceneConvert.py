#!/usr/bin/env python3
import struct
import uuid
import datetime
import base64
import io
import xml.etree.ElementTree as ET
import zlib
import math
import json

class URI(str):
    def __repr__(self):
        return "URI({})".format(super().__repr__())

#Encoders
def llsdEncodeXml(input, destination, *args, optimize = False, encoding = "base64", **kwargs):
    t = type(input)
    if input == None:
        elm = ET.SubElement(destination, "undef")
    elif t == bool:
        elm = ET.SubElement(destination, "boolean")
        if input:
            elm.text = "true"
        elif not optimize:
            elm.text = "false"
    elif t == int:
        elm = ET.SubElement(destination, "integer")
        if input != 0 or not optimize:
            elm.text = str(input)
    elif t == float:
        elm = ET.SubElement(destination, "real")
        if input != 0 or not optimize:
            elm.text = str(input)
    elif t == uuid.UUID:
        elm = ET.SubElement(destination, "uuid")
        if input.bytes != b"\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0" or not optimize:
            elm.text = str(input)
    elif t == str:
        elm = ET.SubElement(destination, "string")
        if input != "" or not optimize:
            elm.text = input
    elif t == bytes:
        encoder = encoding
        elm = ET.SubElement(destination, "binary")
        if input != b"" or not optimize:
            if encoder == "base64":
                elm.text = base64.b64encode(input).decode()
            elif encoder == "base85":
                elm.text = base64.b85encode(input).decode()
            elif encoder == "base16":
                elm.text = base64.b16encode(input).decode()
            else:
                raise ValueError("Unknown binary encoding {}!".format(encoder))
    elif t == datetime.datetime:
        elm = ET.SubElement(destination, "date")
        elm.text = input.strftime("%Y-%m-%dT%H:%M:%S.%fZ")
    elif t == URI:
        elm = ET.SubElement(destination, "uri")
        if input != "" or not optimize:
            elm.text = input
    elif t == dict:
        root = ET.SubElement(destination, "map")
        for key in input:
            if type(key) != str:
                raise ValueError("Dictionary keys must be type str, not {}!".format(type(key)))
            elm = ET.SubElement(root, "key")
            elm.text = key
            llsdEncodeXml(input[key], root, *args, **kwargs)
    elif t == list:
        root = ET.SubElement(destination, "array")
        for value in input:
            llsdEncodeXml(value, root, *args, **kwargs)

def llsdEncodeBytes(input):
    t = type(input)
    if input == None:
        return b"!"
    
    elif t == bool and input == False:
        return b"0"
    
    elif t == bool and input == True:
        return b"1"
    
    elif t == int:
        return b"i" + struct.pack(">i", input)
    
    elif t == float:
        return b"r" + struct.pack(">d", input)
    
    elif t == uuid.UUID:
        return b"u" + input.bytes
    
    elif t == str:
        data = input.encode()
        size = struct.pack(">I", len(data))
        return b"s" + size + data
    
    elif t == bytes:
        size = struct.pack(">I", len(input))
        return b"b" + size + input
    
    elif t == datetime.datetime:
        return b"d" + struct.pack("<d", input.timestamp())
    
    elif t == URI:
        data = str(input).encode()
        size = struct.pack(">I", len(data))
        return b"l" + size + data
    
    elif t == dict:
        size = struct.pack(">I", len(input))
        
        result = bytearray()
        for k, v in input.items():
            result += b"k"
            k = str(k).encode()
            result += struct.pack(">I", len(k)) + k
            value = llsdEncodeBytes(v)
            result += value
        
        return b"{" + size + result + b"}"
    
    elif t == list:
        size = struct.pack(">I", len(input))
        
        result = bytearray()
        for v in input:
            value = llsdEncodeBytes(v)
            result += value
        
        return b"[" + size + result + b"]"
    
    else:
        raise ValueError("Unexpected type {} for LLSD!".format(str(t)))

def llsdEncode(input, *args, format = "xml", **kwargs):
    if format == "xml":
        root = ET.Element("llsd")
        if "optimize" not in kwargs:
            kwargs["optimize"] = True
        llsdEncodeXml(input, root, *args, **kwargs)
        xml = ET.ElementTree(root)
        f = io.BytesIO()
        xml.write(f, encoding='UTF-8', xml_declaration=True)
        return f.getvalue()

#Decoders
def parseISODate(input):
    try:
        if input[-1] == "Z":
            input = input[:-1]
        date, time = input.split("T", 2)
        year, month, day = date.split("-", 3)
        hour, minute, second = time.split(":", 3)
        if "." in second:
            second, microsecond = second.split(".", 2)
        else:
            microsecond = 0
        return datetime.datetime(*[int(i) for i in [year, month, day, hour, minute, second, microsecond]])
    except ValueError:
        raise ValueError("Invalid timestamp '{}'!".format(input))

def llsdDecodeXml(input):
    if input.tag == "undef":
        return None
    elif input.tag == "boolean":
        value = input.text
        if value == None:
            return False
        value = value.lower()
        if value in ["1", "true"]:
            return True
        elif value in ["", "0", "false"]:
            return False
        else:
            raise ValueError("Unexpected value '{}' for boolean!".format(value))
    elif input.tag == "integer":
        if input.text == None:
            return 0
        return int(input.text)
    elif input.tag == "real":
        if input.text == None:
            return 0
        return float(input.text)
    elif input.tag == "uuid":
        if input.text == None:
            return uuid.UUID(bytes=b"\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0")
        return uuid.UUID(input.text)
    elif input.tag == "string":
        if input.text == None:
            return ""
        return input.text
    elif input.tag == "binary":
        if input.text == None:
            return b""
        encoding = input.attrib.get("encoding", "base64").lower()
        if encoding == "base64":
            return base64.b64decode(input.text)
        elif encoding == "base85":
            return base64.b85decode(input.text)
        elif encoding == "base16":
            return base64.b16decode(input.text)
        else:
            raise ValueError("Unknown encoding {} for binary element!".format(encoding))
    elif input.tag == "date":
        if input.text == None:
            return datetime.datetime.fromtimestamp(0)
        return parseISODate(input.text)
    elif input.tag == "uri":
        if input.text == None:
            return URI("")
        return URI(input.text)
    elif input.tag == "map":
        result = {}
        for i in range(0, len(input), 2):
            if input[i].tag != "key":
                raise ValueError("Unexpected {} element in map, expected key!".format(input[i].tag))
            result[input[i].text] = llsdDecodeXml(input[i+1])
        return result
    elif input.tag == "array":
        result = [None]*len(input)
        for i in range(0, len(input)):
            result[i] = llsdDecodeXml(input[i])
        return result
    else:
        raise ValueError("Unexpected {} element in LLSD!".format(input.tag))

def llsdDecodeBytes(input):
    tag = input.read(1)
    if tag == b"!":
        return None
    
    elif tag == b"0":
        return False
    
    elif tag == b"1":
        return True
    
    elif tag == b"i":
        return struct.unpack_from(">i", input.read(4))[0]
    
    elif tag == b"r":
        return struct.unpack_from(">d", input.read(8))[0]
    
    elif tag == b"u":
        return uuid.UUID(bytes=input.read(16))
    
    elif tag == b"s":
        size = struct.unpack_from(">I", input.read(4))[0]
        return input.read(size).decode()
    
    elif tag == b"b":
        size = struct.unpack_from(">I", input.read(4))[0]
        return input.read(size)
    
    elif tag == b"d":
        return datetime.datetime.fromtimestamp(struct.unpack_from("<d", input.read(8))[0])
    
    elif tag == b"l":
        size = struct.unpack_from(">I", input.read(4))[0]
        return URI(input.read(size).decode())
    
    elif tag == b"{":
        size = struct.unpack_from(">I", input.read(4))[0]
        result = {}
        for i in range(0, size):
            if input.read(1) != b"k":
                raise ValueError("Unexpected element in map at {}, expected key!".format(input.tell()-1))
            key = input.read(struct.unpack_from(">I", input.read(4))[0]).decode()
            value = llsdDecodeBytes(input)
            result[key] = value
        if input.read(1) != b"}":
            raise ValueError("Unexpected data at end of map at {}".format(input.tell()-1))
        return result
    
    elif tag == b"[":
        size = struct.unpack_from(">I", input.read(4))[0]
        result = [None] * size
        for i in range(0, size):
            result[i] = llsdDecodeBytes(input)
        if input.read(1) != b"]":
            raise ValueError("Unexpected data at end of list at {}".format(input.tell()-1))
        return result
    
    elif tag == b"}":
        raise ValueError("Unexpected end of map at {}".format(input.tell()-1))
    
    elif tag == b"]":
        raise ValueError("Unexpected end of list at {}".format(input.tell()-1))
    
    else:
        raise ValueError("Unexpected element at {} in LLSD!".format(input.tell()-1))

def llsdDecode(input, *args, format = None, maxHeaderLength = 128, **kwargs):
    if format == None:
        isBytes = type(input) == bytes
        i = 0
        l = len(input)
        while i < l and i < maxHeaderLength:
            if isBytes:
                c = chr(input[i])
            else:
                c = input[i]
            if c == '"' or c == "'":
                quoteChar = c
                while i < l and i < maxHeaderLength:
                    i += 1
                    if isBytes:
                        c = chr(input[i])
                    else:
                        c = input[i]
                    if c == quoteChar:
                        break
                    elif c == "\\":
                        #Assuming the file is valid, no unicode should be in the
                        #header
                        i += 1
                i += 1
            i += 1
            if c == ">":
                break
        header = input[2:i-2].strip().lower()
        if header == "llsd/notation":
            format = "notation"
        elif header == "llsd/binary":
            format = "binary"
        else:
            tmp = header[0:3]
            if type(tmp) == bytes:
                tmp = tmp.decode()
            if tmp == "xml":
                format = "xml"
            else:
                raise ValueError("Unable to detect serialization format!")
    
    if format == "xml":
        input = ET.fromstring(input)
        if input.tag != "llsd":
            raise ValueError("Unexpected tag {} in LLSD+XML!".format(input.tag))
        return llsdDecodeXml(input[0])
    elif format == "llsd":
        return llsdDecodeBytes(input)
    else:
        raise ValueError("Unknown serialization format {}!".format(format))

#Object flags (indra/llprimitive/object_flags.h)
FLAGS_USE_PHYSICS = 1 << 0
FLAGS_PHANTOM = 1 << 10
FLAGS_VOLUME_DETECT = 1 << 14
FLAGS_INCLUDE_IN_SEARCH = 1 << 15
FLAGS_OBJECT_GROUP_OWNED = 1 << 18
FLAGS_TEMPORARY_ON_REZ = 1 << 29

def bytesToInt(value):
    #ll_sd_from_U32 / ll_sd_from_U64 store big endian binary. Read as signed,
    #since LLSD integers are signed 32 bit (masks above 0x7fffffff wrap).
    return int.from_bytes(value, "big", signed = True)

def castLike(default, value):
    #Cast value to the type of default, so the scene's types never change.
    t = type(default)
    if t == bool:
        if type(value) == bytes:
            return bytesToInt(value) != 0
        return bool(value)
    elif t == int:
        if type(value) == bytes:
            return bytesToInt(value)
        return int(value)
    elif t == float:
        if type(value) == bytes:
            return float(bytesToInt(value))
        return float(value)
    elif t == str:
        return str(value)
    elif t == URI:
        return URI(value)
    elif t == bytes:
        if type(value) == bytes:
            return value.rjust(len(default), b"\0") if len(value) < len(default) else value
        if type(value) == int:
            return value.to_bytes(len(default), "big")
        return bytes(value)
    elif t == uuid.UUID:
        if type(value) == uuid.UUID:
            return value
        if type(value) == bytes:
            return uuid.UUID(bytes=value)
        return uuid.UUID(str(value))
    elif t == list:
        if len(default) == 0:
            return list(value)
        return [castLike(default[min(i, len(default) - 1)], v) for i, v in enumerate(value)]
    elif t == dict:
        result = dict(default)
        for key in default:
            if key in value:
                result[key] = castLike(default[key], value[key])
        return result
    return value

def setField(destination, key, value):
    destination[key] = castLike(destination[key], value)

def quaternionToEuler(q):
    #[x, y, z, w] -> [roll, pitch, yaw] in radians, rotations applied X then Y then Z
    x, y, z, w = q
    sinPitch = 2.0 * (w * y + x * z)
    sinPitch = max(-1.0, min(1.0, sinPitch))
    roll = math.atan2(2.0 * (w * x - y * z), w * w - x * x - y * y + z * z)
    pitch = math.asin(sinPitch)
    yaw = math.atan2(2.0 * (w * z - x * y), w * w + x * x - y * y - z * z)
    return [roll, pitch, yaw]

#Particle system source patterns (LLPartSysData::mPattern)
PART_SRC_PATTERNS = [
    ("LL_PART_SRC_PATTERN_DROP", 0x01),
    ("LL_PART_SRC_PATTERN_EXPLODE", 0x02),
    ("LL_PART_SRC_PATTERN_ANGLE", 0x04),
    ("LL_PART_SRC_PATTERN_ANGLE_CONE", 0x08),
    ("LL_PART_SRC_PATTERN_ANGLE_CONE_EMPTY", 0x10)
]

#Per particle flags (LLPartData::mFlags)
PART_FLAGS = [
    ("LL_PART_INTERP_COLOR_MASK", 0x01),
    ("LL_PART_INTERP_SCALE_MASK", 0x02),
    ("LL_PART_BOUNCE_MASK", 0x04),
    ("LL_PART_WIND_MASK", 0x08),
    ("LL_PART_FOLLOW_SRC_MASK", 0x10),
    ("LL_PART_FOLLOW_VELOCITY_MASK", 0x20),
    ("LL_PART_TARGET_POS_MASK", 0x40),
    ("LL_PART_TARGET_LINEAR_MASK", 0x80),
    ("LL_PART_EMISSIVE_MASK", 0x100),
    ("LL_PART_BEAM_MASK", 0x200)
]

#Legacy material fields (scene key, LLMaterial::asLLSD key)
MATERIAL_FIELDS = [
    ("normMap", "NormMap"),
    ("normOffsetX", "NormOffsetX"),
    ("normOffsetY", "NormOffsetY"),
    ("normRepeatX", "NormRepeatX"),
    ("normRepeatY", "NormRepeatY"),
    ("normRotation", "NormRotation"),
    ("specMap", "SpecMap"),
    ("specOffsetX", "SpecOffsetX"),
    ("specOffsetY", "SpecOffsetY"),
    ("specRepeatX", "SpecRepeatX"),
    ("specRepeatY", "SpecRepeatY"),
    ("specRotation", "SpecRotation"),
    ("specColor", "SpecColor"),
    ("specExp", "SpecExp"),
    ("envIntensity", "EnvIntensity"),
    ("alphaMaskCutoff", "AlphaMaskCutoff"),
    ("diffuseAlphaMode", "DiffuseAlphaMode")
]

#GLTF material overrides. Firestorm exports them as the glTF JSON written by
#LLGLTFMaterial::asJSON; the scene stores the sparse override LLSD that
#LLGLTFMaterial::getOverrideLLSD sends (keys tex, bc, ec, mf, rf, am, ac, ds, ti).
GLTF_ALPHA_MODES = {"OPAQUE": 0, "BLEND": 1, "MASK": 2}

def f32(value):
    #Round to single precision, so comparisons match the viewer's F32 fields
    return struct.unpack("<f", struct.pack("<f", float(value)))[0]

def gltfTextureInfo(model, info):
    #Returns (texture UUID, (offset, scale, rotation)) for a glTF textureInfo
    textureId = uuid.UUID(int=0)
    offset = [0.0, 0.0]
    scale = [1.0, 1.0]
    rotation = 0.0
    if info == None:
        return textureId, (offset, scale, rotation)
    try:
        texture = model["textures"][info["index"]]
        textureId = uuid.UUID(model["images"][texture["source"]]["uri"])
    except (KeyError, IndexError, TypeError, ValueError):
        pass
    transform = info.get("extensions", {}).get("KHR_texture_transform")
    if type(transform) == dict:
        if len(transform.get("offset", [])) >= 2:
            offset = [f32(v) for v in transform["offset"][:2]]
        if len(transform.get("scale", [])) >= 2:
            scale = [f32(v) for v in transform["scale"][:2]]
        if "rotation" in transform:
            rotation = f32(transform["rotation"])
    return textureId, (offset, scale, rotation)

def setSparse(array, index, value):
    #LLSD arrays grow with undef entries when a later index is assigned
    while len(array) <= index:
        array.append(None)
    array[index] = value

def gltfOverrideLLSD(source):
    model = json.loads(source) if type(source) == str else source
    materials = model.get("materials", [])
    if len(materials) == 0:
        return None
    material = materials[0]
    pbr = material.get("pbrMetallicRoughness", {})
    extras = material.get("extras", {})
    if type(extras) != dict:
        extras = {}
    
    #Texture slots, in LLGLTFMaterial::TextureInfo order
    textures = [
        gltfTextureInfo(model, pbr.get("baseColorTexture")),
        gltfTextureInfo(model, material.get("normalTexture")),
        gltfTextureInfo(model, pbr.get("metallicRoughnessTexture")),
        gltfTextureInfo(model, material.get("emissiveTexture"))
    ]
    
    baseColor = [f32(v) for v in pbr.get("baseColorFactor", [1.0, 1.0, 1.0, 1.0])]
    emissiveColor = [f32(v) for v in material.get("emissiveFactor", [0.0, 0.0, 0.0])]
    metallic = f32(max(0.0, min(1.0, pbr.get("metallicFactor", 1.0))))
    roughness = f32(max(0.0, min(1.0, pbr.get("roughnessFactor", 1.0))))
    alphaMode = GLTF_ALPHA_MODES.get(material.get("alphaMode", "OPAQUE"), 0)
    alphaCutoff = f32(max(0.0, min(1.0, material.get("alphaCutoff", 0.5))))
    doubleSided = bool(material.get("doubleSided", False))
    overrideAlphaMode = bool(extras.get("override_alpha_mode", False))
    overrideDoubleSided = bool(extras.get("override_double_sided", False))
    
    #Only values that differ from the defaults are kept. The base material is
    #unknown here, so texture IDs are compared against null.
    data = {}
    for i, (textureId, transform) in enumerate(textures):
        if textureId.int != 0:
            data.setdefault("tex", [])
            setSparse(data["tex"], i, textureId)
    if baseColor != [1.0, 1.0, 1.0, 1.0]:
        data["bc"] = baseColor
    if emissiveColor != [0.0, 0.0, 0.0]:
        data["ec"] = emissiveColor
    if metallic != 1.0:
        data["mf"] = metallic
    if roughness != 1.0:
        data["rf"] = roughness
    if alphaMode != 0 or overrideAlphaMode:
        data["am"] = alphaMode
    if alphaCutoff != f32(0.5):
        data["ac"] = alphaCutoff
    if doubleSided or overrideDoubleSided:
        data["ds"] = doubleSided
    for i, (textureId, (offset, scale, rotation)) in enumerate(textures):
        entry = {}
        if offset != [0.0, 0.0]:
            entry["o"] = offset
        if scale != [1.0, 1.0]:
            entry["s"] = scale
        if rotation != 0.0:
            entry["r"] = rotation
        if len(entry) > 0:
            data.setdefault("ti", [])
            setSparse(data["ti"], i, entry)
    return data

def fillParticles(destination, particle):
    #Keys match LLPartSysData::unpackSystem and LLPartData::unpackLegacy.
    #CRC, source flags and acceleration are not exported; any nonzero CRC
    #marks the system present.
    partFlags = 0
    for key, bit in PART_FLAGS:
        if particle.get(key, 0):
            partFlags |= bit
    pattern = 0
    for key, bit in PART_SRC_PATTERNS:
        if particle.get(key, 0):
            pattern |= bit
    setField(destination, "pscrc", 1)
    setField(destination, "pspattern", pattern)
    setField(destination, "psmaxage", particle["SourceMaxAge"])
    setField(destination, "psstartage", particle["SourceStartAge"])
    setField(destination, "psinnerangle", particle["InnerAngle"])
    setField(destination, "psouterangle", particle["OuterAngle"])
    setField(destination, "psburstrate", max(0.01, float(particle["BurstRate"])))
    setField(destination, "psburstradius", particle["BurstRadius"])
    setField(destination, "psburstspeedmin", particle["BurstSpeedMin"])
    setField(destination, "psburstspeedmax", particle["BurstSpeedMax"])
    setField(destination, "psburstpartcount", particle["BurstPartCount"])
    setField(destination, "psangvelx", particle["AngularVelocity"][0])
    setField(destination, "psangvely", particle["AngularVelocity"][1])
    setField(destination, "psangvelz", particle["AngularVelocity"][2])
    setField(destination, "psuuid", particle["PartImageID"])
    setField(destination, "pstargetuuid", particle["TargetId"])
    
    setField(destination, "pdflags", partFlags)
    setField(destination, "pdmaxage", particle["ParticleMaxAge"])
    setField(destination, "pdstartcolor", particle["StartColor"])
    setField(destination, "pdendcolor", particle["EndColor"])
    setField(destination, "pdstartscalex", particle["StartScale"][0])
    setField(destination, "pdstartscaley", particle["StartScale"][1])
    setField(destination, "pdendscalex", particle["EndScale"][0])
    setField(destination, "pdendscaley", particle["EndScale"][1])
    
    #Firestorm's LLPartSysData::asLLSD does not write acceleration, the blend
    #functions or glow, so these are the viewer's defaults (no acceleration,
    #LL_PART_BF_SOURCE_ALPHA / LL_PART_BF_ONE_MINUS_SOURCE_ALPHA, no glow).
    setField(destination, "psaccelx", 0.0)
    setField(destination, "psaccely", 0.0)
    setField(destination, "psaccelz", 0.0)
    setField(destination, "pdblendsource", 7)
    setField(destination, "pdblenddest", 9)
    setField(destination, "pdstartglow", 0.0)
    setField(destination, "pdendglow", 0.0)

def convertInventoryItem(item):
    #Same fields as an LLEmbeddedItems inv_item (LLInventoryItem legacy stream)
    result = {
        "item_id": uuid.UUID("00000000-0000-0000-0000-000000000000"),
        "parent_id": uuid.UUID("00000000-0000-0000-0000-000000000000"),
        "permissions": {
            "base_mask": 0,
            "owner_mask": 0,
            "group_mask": 0,
            "everyone_mask": 0,
            "next_owner_mask": 0,
            "creator_id": uuid.UUID("00000000-0000-0000-0000-000000000000"),
            "owner_id": uuid.UUID("00000000-0000-0000-0000-000000000000"),
            "last_owner_id": uuid.UUID("00000000-0000-0000-0000-000000000000"),
            "group_id": uuid.UUID("00000000-0000-0000-0000-000000000000"),
            "group_owned": False
        },
        "asset_id": uuid.UUID("00000000-0000-0000-0000-000000000000"),
        "type": "",
        "inv_type": "",
        "flags": 0,
        "sale_info": {
            "sale_type": "",
            "sale_price": 0
        },
        "name": "",
        "desc": "",
        "creation_date": 0
    }
    for key in ["item_id", "parent_id", "asset_id", "type", "inv_type", "name", "desc", "sale_info"]:
        if key in item:
            setField(result, key, item[key])
    if "flags" in item:
        setField(result, "flags", item["flags"])
    if "created_at" in item:
        setField(result, "creation_date", item["created_at"])
    permissions = item.get("permissions", {})
    for key in result["permissions"]:
        if key in permissions:
            setField(result["permissions"], key, permissions[key])
    if "is_owner_group" in permissions:
        setField(result["permissions"], "group_owned", permissions["is_owner_group"])
    return result

#Terrain
REGION_WIDTH = 256
RAW_CHANNELS = 13

def loadRawHeights(path):
    #Second Life terrain RAW: 256x256 pixels, 13 interleaved 8 bit channels.
    #Channel 1 is height, channel 2 a multiplier (height = c1 * c2 / 128).
    #The first row in the file is the north edge of the region.
    with open(path, "rb") as f:
        data = f.read()
    expected = REGION_WIDTH * REGION_WIDTH * RAW_CHANNELS
    if len(data) != expected:
        raise ValueError("{} is {} bytes; a 256x256 RAW with 13 channels is {} bytes!".format(path, len(data), expected))
    heights = [0.0] * (REGION_WIDTH * REGION_WIDTH)
    for row in range(REGION_WIDTH):
        y = REGION_WIDTH - 1 - row
        for x in range(REGION_WIDTH):
            i = (row * REGION_WIDTH + x) * RAW_CHANNELS
            heights[y * REGION_WIDTH + x] = data[i] * data[i + 1] / 128.0
    return heights

def loadTerrain(value):
    #A number gives flat terrain at that height, anything else is a RAW file
    try:
        height = float(value)
    except ValueError:
        return loadRawHeights(value)
    return [height] * (REGION_WIDTH * REGION_WIDTH)

def packHeights(heights):
    #Heights as little endian float32, row by row from the south edge
    #(index = y * 256 + x)
    return struct.pack("<{}f".format(len(heights)), *heights)

def convertScene(input, heights = None, water = 20.0):
    if heights == None:
        heights = loadTerrain(20.0)
    scene = {
        "_version": 1,
        "asset_cdn": "http://asset-cdn.glb.agni.lindenlab.com",
        "scene": {
            "environment": {
                "water": uuid.UUID("f76ad270-ef02-50f8-4032-ae9fce7e51c0"),
                "skies": [
                    {
                        "height": 3000,
                        "sky": uuid.UUID("e23c3070-1342-4130-f733-76ce7a60634f")
                    },
                    {
                        "height": 2000,
                        "sky": uuid.UUID("e23c3070-1342-4130-f733-76ce7a60634f")
                    },
                    {
                        "height": 1000,
                        "sky": uuid.UUID("e23c3070-1342-4130-f733-76ce7a60634f")
                    },
                    {
                        "height": 0,
                        "sky": uuid.UUID("e23c3070-1342-4130-f733-76ce7a60634f")
                    }
                ],
                "dayLength": 4.0,
                "dayOffset": -8.0
            },
            "terrain": {
                "water": float(water),
                "textures": {
                    "base": [
                        uuid.UUID("b8d3965a-ad78-bf43-699b-bff8eca6c975"),
                        uuid.UUID("abb783e6-3e93-26c0-248a-247666855da3"),
                        uuid.UUID("179cdabd-398a-9b6b-1391-4dc333ba321f"),
                        uuid.UUID("beb169c7-11ea-fff2-efe5-0f24dc881df2")
                    ],
                    "detail": [
                        uuid.UUID("0bc58228-74a0-7e83-89bc-5c23464bcec5"),
                        uuid.UUID("63338ede-0037-c4fd-855b-015d77112fc8"),
                        uuid.UUID("303cd381-8560-7579-23f1-f0a880799740"),
                        uuid.UUID("53a2f406-4895-1d13-d541-d2e3b86bc19c")
                    ],
                    "startHeights": [
                        20.0,
                        20.0,
                        20.0,
                        20.0
                    ],
                    "heightRanges": [
                        60.0,
                        60.0,
                        60.0,
                        60.0,
                    ]
                },
                "heights": packHeights(heights)
            },
            "objects": [
                
            ]
        }
    }
    
    for linkset in input["linkset"]:
        objs = []
        for link in linkset:
            prim = input["prim"][str(link)]
            obj = {
                "angvel": [0.0, 0.0, 0.0],
                "birthtime": b"\0" * 8,
                "block_grabs": 0,
                "camera_at_offset": [0.0, 0.0, 0.0],
                "camera_eye_offset": [0.0, 0.0, 0.0],
                "cast_shadows": 0,
                "click_action": 0,
                "correct_family_id": 0,
                "default_pay_price": b"\0" * 4,
                "delay": b"\0" * 8,
                "description": "",
                "die_at_edge": 0,
                "displayopts": 0,
                "displaytype": 0,
                "energy": 1.0,
                "ExtraPhysics": {
                    "physicsShapeType": 0,
                    "density": 1000.0,
                    "friction": 0.6,
                    "restitution": 0.5,
                    "gravityMultiplier": 1.0
                },
                "extra_parameters": {
                    "flexible": None if "flexible" not in prim else {
                        "tension": 0,
                        "drag": 0,
                        "gravity": 0,
                        "wind": 0,
                        "userforce": [0.0, 0.0, 0.0]
                    },
                    "light": None if "light" not in prim else {
                        "color": [0.0, 0.0, 0.0, 0.0],
                        "radius": 0.0,
                        "cutoff": 0.0,
                        "falloff": 0.0
                    },
                    "sculpted": None if "sculpt" not in prim else {
                        "texture": uuid.UUID("00000000-0000-0000-0000-000000000000"),
                        "type": 0
                    },
                    "lightimage": {
                        "texture": uuid.UUID("00000000-0000-0000-0000-000000000000"),
                        "params": [0.0, 0.0, 0.0]
                    }
                },
                "facelist": [
                    {
                        "bump": 0,
                        "color": [0.0, 0.0, 0.0, 0.0],
                        "fullbright": 0,
                        "glow": 0.0,
                        "imageid": uuid.UUID("00000000-0000-0000-0000-000000000000"),
                        "imagerot": 0.0,
                        "media_flags": 0,
                        "offsets": 0.0,
                        "offsett": 0.0,
                        "scales": 0.0,
                        "scalet": 0.0,
                        "media_data": {
                            "alt_image_enable": False,
                            "controls": 0,
                            "current_url": "",
                            "home_url": "",
                            "auto_loop": False,
                            "auto_play": False,
                            "auto_scale": False,
                            "auto_zoom": False,
                            "first_click_interact": False,
                            "width_pixels": 0,
                            "height_pixels": 0,
                            "whitelist_enable": False,
                            "whitelist": [],
                            "perms_interact": 0,
                            "perms_control": 0
                        },
                        "material": {
                            "normMap": uuid.UUID("00000000-0000-0000-0000-000000000000"),
                            "normOffsetX": 0,
                            "normOffsetY": 0,
                            "normRepeatX": 0,
                            "normRepeatY": 0,
                            "normRotation": 0,
                            "specMap": uuid.UUID("00000000-0000-0000-0000-000000000000"),
                            "specOffsetX": 0,
                            "specOffsetY": 0,
                            "specRepeatX": 0,
                            "specRepeatY": 0,
                            "specRotation": 0,
                            "specColor": [0, 0, 0, 0],
                            "specExp": 0,
                            "envIntensity": 0,
                            "alphaMaskCutoff": 0,
                            "diffuseAlphaMode": 0
                        },
                        "gltfMaterial": None
                    }
                ],
                "faces": 0,
                "family_generation": 0,
                "gpw_bias": 0.0,
                "has_rezzed": 1,
                "include_in_search_v2": 0,
                "inventory": [],
                "local_id": b"\0" * 4,
                "lslmaps": None,
                "material": 0,
                "maxenergy": 1.0,
                "name": "",
                "namevalues": {},
                "nextstart": b"\0" * 8,
                "oldpos": [0.0, 0.0, 0.0],
                "parceltime": b"\0" * 8,
                "particles": None if "particle" not in prim else {
                    "pscrc": 0,
                    "psflags": 0,
                    "pspattern": 0,
                    "psmaxage": 0.0,
                    "psstartage": 0.0,
                    "psinnerangle": 0.0,
                    "psouterangle": 0.0,
                    "psburstrate": 0.0,
                    "psburstradius": 0.0,
                    "psburstspeedmin": 0.0,
                    "psburstspeedmax": 0.0,
                    "psburstpartcount": 0,
                    "psangvelx": 0.0,
                    "psangvely": 0.0,
                    "psangvelz": 0.0,
                    "psaccelx": 0.0,
                    "psaccely": 0.0,
                    "psaccelz": 0.0,
                    "psuuid": uuid.UUID("00000000-0000-0000-0000-000000000000"),
                    "pstargetuuid": uuid.UUID("00000000-0000-0000-0000-000000000000"),
                    "pdflags": 0,
                    "pdmaxage": 0.0,
                    "pdstartcolor": [0.0, 0.0, 0.0, 0.0],
                    "pdendcolor": [0.0, 0.0, 0.0, 0.0],
                    "pdstartscalex": 0.0,
                    "pdstartscaley": 0.0,
                    "pdendscalex": 0.0,
                    "pdendscaley": 0.0,
                    "pdblendsource": 0,
                    "pdblenddest": 0,
                    "pdstartglow": 0.0,
                    "pdendglow": 0.0
                },
                "pay_button_0": 0,
                "pay_button_1": 0,
                "pay_button_2": 0,
                "pay_button_3": 0,
                "permissions": {
                    "base_mask": 0,
                    "creator_id": uuid.UUID("00000000-0000-0000-0000-000000000000"),
                    "everyone_mask": 0,
                    "group_id": uuid.UUID("00000000-0000-0000-0000-000000000000"),
                    "group_mask": 0,
                    "is_owner_group": False,
                    "last_owner_id": uuid.UUID("00000000-0000-0000-0000-000000000000"),
                    "next_owner_mask": 0,
                    "owner_id": uuid.UUID("00000000-0000-0000-0000-000000000000"),
                    "owner_mask": 0
                },
                "phantom": 0,
                "pos": [0.0, 0.0, 0.0],
                "pre_link_base_mask": b"\0" * 4,
                "ps_next_crc": b"\0" * 4,
                "remote_script_access_pin": 0,
                "return_at_edge": 0,
                "reztime": b"\0" * 8,
                "rotate_x": 0,
                "rotate_y": 0,
                "rotate_z": 0,
                "rotation": [0.0, 0.0, 0.0, 0.0],
                "saleinfo": {
                    "sale_price": 0,
                    "sale_type": ""
                },
                "sandbox": 0,
                "sandboxhome": [0.0, 0.0, 0.0],
                "scale": [0.0, 0.0, 0.0],
                "scratchpad": b"",
                "selected": False,
                "selector": uuid.UUID("00000000-0000-0000-0000-000000000000"),
                "shape": {
                    "path": {
                        "begin": 0.0,
                        "curve": 0,
                        "end": 0.0,
                        "radius_offset": 0.0,
                        "revolutions": 0.0,
                        "scale_x": 0.0,
                        "scale_y": 0.0,
                        "scale_z": 0.0,
                        "shear_x": 0.0,
                        "shear_y": 0.0,
                        "skew": 0.0,
                        "taper_x": 0.0,
                        "taper_y": 0.0,
                        "twist": 0.0,
                        "twist_begin": 0.0
                    },
                    "profile": {
                        "begin": 0.0,
                        "curve": 0,
                        "end": 0.0,
                        "hollow": 0.0
                    }
                },
                "sit_hint": 0,
                "sit_name": "",
                "sit_offset": [0.0, 0.0, 0.0],
                "sit_quad": [0.0, 0.0, 0.0, 0.0],
                "soundflags": 0,
                "soundgain": 0.0,
                "soundid": uuid.UUID("00000000-0000-0000-0000-000000000000"),
                "soundradius": 0.0,
                "state": 0,
                "task_valid": 0,
                "tax_rate": 0,
                "temporary": 0,
                "textcolor": [0.0, 0.0, 0.0, 0.0],
                "total_crc": b"\0" * 4,
                "touch_name": "",
                "travel_access": 0,
                "type": 0x09,
                "usephysics": 0,
                "velocity": [0.0, 0.0, 0.0],
                "volume_detect": 1
            }
            
            #Transform (children are relative to the root)
            setField(obj, "pos", prim["position"])
            setField(obj, "rotation", prim["rotation"])
            setField(obj, "scale", prim["scale"])
            
            #Object flags
            flags = castLike(0, prim.get("flags", 0))
            setField(obj, "usephysics", (flags & FLAGS_USE_PHYSICS) != 0)
            setField(obj, "phantom", (flags & FLAGS_PHANTOM) != 0)
            setField(obj, "volume_detect", (flags & FLAGS_VOLUME_DETECT) != 0)
            setField(obj, "include_in_search_v2", (flags & FLAGS_INCLUDE_IN_SEARCH) != 0)
            setField(obj, "temporary", (flags & FLAGS_TEMPORARY_ON_REZ) != 0)
            
            if "clickaction" in prim:
                setField(obj, "click_action", prim["clickaction"])
            
            #Shape
            setField(obj["shape"], "path", prim["volume"]["path"])
            setField(obj["shape"], "profile", prim["volume"]["profile"])
            
            #Extra parameters
            extra = obj["extra_parameters"]
            if "flexible" in prim:
                flexible = prim["flexible"]
                setField(extra["flexible"], "tension", flexible["tension"])
                setField(extra["flexible"], "drag", flexible["air_friction"])
                setField(extra["flexible"], "gravity", flexible["gravity"])
                setField(extra["flexible"], "wind", flexible["wind_sensitivity"])
                setField(extra["flexible"], "userforce", flexible["user_force"])
            if "light" in prim:
                setField(extra, "light", prim["light"])
            if "sculpt" in prim:
                setField(extra, "sculpted", prim["sculpt"])
            if "light_texture" in prim:
                setField(extra, "lightimage", prim["light_texture"])
            
            #Faces
            faceDefault = obj["facelist"][0]
            facelist = []
            materials = prim.get("materials", [])
            for i, te in enumerate(prim.get("texture", [])):
                face = dict(faceDefault)
                for key in faceDefault:
                    if key in te:
                        setField(face, key, te[key])
                setField(face, "color", te["colors"])
                if "media_data" not in te:
                    #Shared media is only present on faces that have it
                    face["media_data"] = None
                #Legacy materials (RenderMaterials cap format). The export
                #keeps no face index; like Firestorm's importer, entry i
                #belongs to face i.
                if i < len(materials):
                    renamed = {key: materials[i][source] for key, source in MATERIAL_FIELDS if source in materials[i]}
                    setField(face, "material", renamed)
                else:
                    face["material"] = None
                if "gltf_override" in te:
                    face["gltfMaterial"] = gltfOverrideLLSD(te["gltf_override"])
                facelist.append(face)
            if len(facelist) > 0:
                obj["facelist"] = facelist
            else:
                faceDefault["media_data"] = None
                faceDefault["material"] = None
            setField(obj, "faces", len(facelist))
            
            #Description
            setField(obj, "name", prim.get("name", ""))
            setField(obj, "description", prim.get("description", ""))
            setField(obj, "touch_name", prim.get("touch_name", ""))
            setField(obj, "sit_name", prim.get("sit_name", ""))
            if "creation_date" in prim:
                setField(obj, "birthtime", prim["creation_date"])
            
            #Physics
            if "material" in prim:
                setField(obj, "material", prim["material"])
            if "ExtraPhysics" in prim:
                physics = prim["ExtraPhysics"]
                setField(obj["ExtraPhysics"], "physicsShapeType", physics["PhysicsShapeType"])
                setField(obj["ExtraPhysics"], "density", physics["Density"])
                setField(obj["ExtraPhysics"], "friction", physics["Friction"])
                setField(obj["ExtraPhysics"], "restitution", physics["Restitution"])
                setField(obj["ExtraPhysics"], "gravityMultiplier", physics["GravityMultiplier"])
            
            if "particle" in prim:
                fillParticles(obj["particles"], prim["particle"])
            
            #Attachments: the root's position and rotation are relative to the
            #attachment point
            if "attachment_point" in prim:
                obj["namevalues"] = {
                    "AttachPt": castLike(0, prim["attachment_point"]),
                    "AttachmentOrientation": castLike([0.0], quaternionToEuler(prim["rotation"])),
                    "AttachmentOffset": castLike([0.0], prim["position"])
                }
            
            #Contents
            if "content" in prim:
                for itemId in prim["content"]:
                    item = input.get("inventory", {}).get(str(itemId))
                    if item != None:
                        obj["inventory"].append(convertInventoryItem(item))
            
            #Permissions
            permissions = obj["permissions"]
            for key in ["creator_id", "owner_id", "group_id", "last_owner_id",
                        "base_mask", "owner_mask", "group_mask", "everyone_mask", "next_owner_mask"]:
                if key in prim:
                    setField(permissions, key, prim[key])
            setField(permissions, "is_owner_group", (flags & FLAGS_OBJECT_GROUP_OWNED) != 0)
            
            if "sale_info" in prim:
                setField(obj, "saleinfo", prim["sale_info"])
            
            objs.append(obj)
        
        scene["scene"]["objects"].append(objs)
    
    return scene

def main():
    import argparse
    parser = argparse.ArgumentParser(
                    prog='ProgramName',
                    description='What the program does',
                    epilog='Text at the bottom of help')
    parser.add_argument('input')
    parser.add_argument('output')
    parser.add_argument('--terrain', default = "20.0",
                        help = "Second Life terrain .raw file, or a height in meters for flat terrain (default 20.0)")
    parser.add_argument('--water', type = float, default = 20.0,
                        help = "water height in meters (default 20.0)")
    args = parser.parse_args()
    heights = loadTerrain(args.terrain)
    with open(args.input, "rb") as f:
        data = llsdDecodeBytes(io.BytesIO(zlib.decompress(f.read())))
    
    data = convertScene(data, heights, args.water)
    data = llsdEncodeBytes(data)
    with open(args.output, "wb") as f:
        f.write(data)

if __name__ == "__main__":
    main()