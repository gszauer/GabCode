// Minimal ZIP writer — STORED (uncompressed) format only. Builds a valid
// .zip archive as a Uint8Array from an array of {path, content} entries,
// where content is a string (UTF-8 encoded) or a Uint8Array (raw bytes).
//
// Files we care about in a chat's VFS are text (prompts/configs/history/md)
// so compression adds little; the STORED format keeps the implementation to
// under ~100 lines with no third-party dependency.

// CRC-32 / IEEE 802.3 polynomial 0xEDB88320, table-based. Cached on first use.
let crcTable = null;
function getCrcTable() {
    if (crcTable) return crcTable;
    const t = new Uint32Array(256);
    for (let n = 0; n < 256; ++n) {
        let c = n;
        for (let k = 0; k < 8; ++k) {
            c = (c & 1) ? (0xEDB88320 ^ (c >>> 1)) : (c >>> 1);
        }
        t[n] = c;
    }
    crcTable = t;
    return t;
}

function crc32(data) {
    const t = getCrcTable();
    let crc = 0xFFFFFFFF;
    for (let i = 0; i < data.length; ++i) {
        crc = t[(crc ^ data[i]) & 0xFF] ^ (crc >>> 8);
    }
    return (crc ^ 0xFFFFFFFF) >>> 0;
}

// ZIP uses MS-DOS packed date/time. Year offset from 1980, seconds halved.
function dosDateTime(d) {
    const date = ((d.getFullYear() - 1980) << 9) | ((d.getMonth() + 1) << 5) | d.getDate();
    const time = (d.getHours() << 11) | (d.getMinutes() << 5) | Math.floor(d.getSeconds() / 2);
    return { date, time };
}

// Accepts [{path: string, content: string | Uint8Array}], returns Uint8Array
// containing a complete zip archive (local headers + central directory + EOCD).
export function createZip(files) {
    const encoder = new TextEncoder();
    const { date: dosDate, time: dosTime } = dosDateTime(new Date());

    const chunks = [];
    const entries = [];
    let offset = 0;

    for (const file of files) {
        const nameBytes = encoder.encode(file.path);
        const data = typeof file.content === 'string'
            ? encoder.encode(file.content)
            : file.content;
        const crc = crc32(data);
        const size = data.length;

        // Local file header: 30 bytes + filename
        const lfh = new Uint8Array(30 + nameBytes.length);
        const lv = new DataView(lfh.buffer);
        lv.setUint32(0, 0x04034b50, true);  // signature
        lv.setUint16(4, 20, true);          // version needed
        lv.setUint16(6, 0, true);           // general purpose flags
        lv.setUint16(8, 0, true);           // compression method (STORED)
        lv.setUint16(10, dosTime, true);
        lv.setUint16(12, dosDate, true);
        lv.setUint32(14, crc, true);
        lv.setUint32(18, size, true);       // compressed size
        lv.setUint32(22, size, true);       // uncompressed size
        lv.setUint16(26, nameBytes.length, true);
        lv.setUint16(28, 0, true);          // extra field length
        lfh.set(nameBytes, 30);

        chunks.push(lfh, data);
        entries.push({ nameBytes, crc, size, offset });
        offset += lfh.length + data.length;
    }

    // Central directory
    const cdStart = offset;
    for (const e of entries) {
        const cd = new Uint8Array(46 + e.nameBytes.length);
        const dv = new DataView(cd.buffer);
        dv.setUint32(0, 0x02014b50, true);  // signature
        dv.setUint16(4, 20, true);          // version made by
        dv.setUint16(6, 20, true);          // version needed
        dv.setUint16(8, 0, true);           // general purpose flags
        dv.setUint16(10, 0, true);          // compression method
        dv.setUint16(12, dosTime, true);
        dv.setUint16(14, dosDate, true);
        dv.setUint32(16, e.crc, true);
        dv.setUint32(20, e.size, true);
        dv.setUint32(24, e.size, true);
        dv.setUint16(28, e.nameBytes.length, true);
        dv.setUint16(30, 0, true);          // extra length
        dv.setUint16(32, 0, true);          // comment length
        dv.setUint16(34, 0, true);          // disk number start
        dv.setUint16(36, 0, true);          // internal file attrs
        dv.setUint32(38, 0, true);          // external file attrs
        dv.setUint32(42, e.offset, true);
        cd.set(e.nameBytes, 46);
        chunks.push(cd);
        offset += cd.length;
    }
    const cdSize = offset - cdStart;

    // End of central directory record
    const eocd = new Uint8Array(22);
    const ev = new DataView(eocd.buffer);
    ev.setUint32(0, 0x06054b50, true);      // signature
    ev.setUint16(4, 0, true);               // disk number
    ev.setUint16(6, 0, true);               // disk with CD
    ev.setUint16(8, entries.length, true);  // entries on this disk
    ev.setUint16(10, entries.length, true); // total entries
    ev.setUint32(12, cdSize, true);
    ev.setUint32(16, cdStart, true);
    ev.setUint16(20, 0, true);              // comment length
    chunks.push(eocd);

    // Concatenate
    const total = chunks.reduce((n, c) => n + c.length, 0);
    const out = new Uint8Array(total);
    let pos = 0;
    for (const c of chunks) { out.set(c, pos); pos += c.length; }
    return out;
}

// Convenience: build a zip of all files in a VFS (skips empty directories)
// and trigger a browser download.
export async function downloadVfsAsZip(vfs, filename) {
    const tree = await vfs.walk('');
    const files = [];
    for (const entry of tree) {
        if (entry.isDir) continue;
        const content = await vfs.readFile(entry.path);
        if (content === null) continue;
        files.push({ path: entry.path, content });
    }
    const bytes = createZip(files);
    const blob = new Blob([bytes], { type: 'application/zip' });
    const url = URL.createObjectURL(blob);
    const a = document.createElement('a');
    a.href = url;
    a.download = filename;
    document.body.appendChild(a);
    a.click();
    document.body.removeChild(a);
    URL.revokeObjectURL(url);
}
