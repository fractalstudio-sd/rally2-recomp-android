package org.srally2;

import java.io.File;
import java.io.IOException;
import java.io.InputStream;
import java.io.RandomAccessFile;
import java.nio.MappedByteBuffer;
import java.nio.channels.FileChannel;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.zip.CRC32;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;

/**
 * Builds the flat region images the native game loads (crom.bin, vrom.bin, ...) straight from the user's srally2.zip.
 * Same result, byte for byte, as tools/build_images.py. Every file is CRC32-checked. Plain java.* only (testable on a desktop JVM).
 */
final class RomBuilder {
    interface Progress { void onProgress(String what, int done, int total); }

    static final class BadRomException extends IOException { BadRomException(String m) { super(m); } }

    /**
     * The images directory is usable when every region file is there and no build was interrupted
     * (a folder copied over by push_images.sh has no marker files at all and is accepted as is).
     */
    static boolean isComplete(File dir) {
        if (new File(dir, ".building").exists()) return false;
        for (String n : RomLayout.REQUIRED) if (!new File(dir, n + ".bin").isFile()) return false;
        return true;
    }

    private static final class Slot { final RomLayout.R region; final RomLayout.F file; Slot(RomLayout.R r, RomLayout.F f) { region = r; file = f; } }

    static void build(InputStream zip, File outDir, Progress cb) throws IOException {
        if (!outDir.isDirectory() && !outDir.mkdirs()) throw new IOException("cannot create " + outDir);
        for (RomLayout.R r : RomLayout.REGIONS) new File(outDir, r.name + ".bin").delete();
        if (!new File(outDir, ".building").createNewFile() && !new File(outDir, ".building").exists()) throw new IOException("cannot write marker");

        Map<String, List<Slot>> byName = new HashMap<>();
        int total = 0;
        for (RomLayout.R r : RomLayout.REGIONS)
            for (RomLayout.F f : r.files) {
                List<Slot> l = byName.get(f.name);
                if (l == null) { l = new ArrayList<>(); byName.put(f.name, l); }
                l.add(new Slot(r, f)); total++;
            }

        Map<String, MappedByteBuffer> maps = new HashMap<>();
        Map<String, RandomAccessFile> raf = new HashMap<>();
        Map<String, Long> sizes = new HashMap<>();
        boolean ok = false;
        try {
            int done = 0;
            byte[] buf = new byte[1 << 16];
            ZipInputStream zin = new ZipInputStream(zip);
            ZipEntry e;
            java.io.ByteArrayOutputStream bos = new java.io.ByteArrayOutputStream(1 << 22);
            java.util.Set<String> seen = new java.util.HashSet<>();
            while ((e = zin.getNextEntry()) != null) {
                String nm = e.getName();
                int sl = nm.lastIndexOf('/'); if (sl >= 0) nm = nm.substring(sl + 1);
                List<Slot> slots = byName.get(nm);
                if (slots == null || e.isDirectory()) continue;
                bos.reset();
                CRC32 crc = new CRC32();
                int n;
                while ((n = zin.read(buf)) > 0) { bos.write(buf, 0, n); crc.update(buf, 0, n); }
                if (crc.getValue() != slots.get(0).file.crc)
                    throw new BadRomException("CRC errato in " + nm + ": non è il set ROM atteso (srally2, versione Export)");
                byte[] data = bos.toByteArray();
                for (Slot s : slots) {
                    if (cb != null) cb.onProgress(s.region.name, done, total);
                    MappedByteBuffer mb = region(s.region, data, outDir, maps, raf, sizes, byName);
                    place(mb, s.region, s.file, data);
                    done++;
                }
                seen.add(nm);
            }
            StringBuilder missing = new StringBuilder();
            for (String k : byName.keySet()) if (!seen.contains(k)) missing.append(' ').append(k);
            if (missing.length() > 0) throw new BadRomException("Nello zip mancano questi file:" + missing);
            for (MappedByteBuffer mb : maps.values()) mb.force();
            ok = true;
        } finally {
            for (RandomAccessFile f : raf.values()) try { f.close(); } catch (IOException ignored) { }
            if (!ok) for (RomLayout.R r : RomLayout.REGIONS) new File(outDir, r.name + ".bin").delete();   // keeps .building: never accepted half-built
        }
        // every region must have been created (a region is created when its first file shows up)
        for (RomLayout.R r : RomLayout.REGIONS) if (!new File(outDir, r.name + ".bin").isFile()) throw new BadRomException("region " + r.name + " vuota");
        new File(outDir, ".building").delete();
    }

    /** Region size exactly as tools/build_images.py computes it (file lengths are part of the layout, they are fixed by the CRCs). */
    private static long regionSize(RomLayout.R r) {
        long size = 0;
        for (RomLayout.F f : r.files) size = Math.max(size, f.off + ((long) f.len / r.chunk - 1) * r.stride + r.chunk);
        return ((size + r.stride - 1) / r.stride) * r.stride;
    }

    private static MappedByteBuffer region(RomLayout.R r, byte[] firstData, File dir, Map<String, MappedByteBuffer> maps,
                                           Map<String, RandomAccessFile> raf, Map<String, Long> sizes, Map<String, List<Slot>> byName) throws IOException {
        MappedByteBuffer mb = maps.get(r.name);
        if (mb != null) return mb;
        long size = regionSize(r);
        RandomAccessFile f = new RandomAccessFile(new File(dir, r.name + ".bin"), "rw");
        f.setLength(0); f.setLength(size);
        raf.put(r.name, f);
        mb = f.getChannel().map(FileChannel.MapMode.READ_WRITE, 0, size);
        maps.put(r.name, mb);
        sizes.put(r.name, size);
        return mb;
    }

    private static void place(MappedByteBuffer mb, RomLayout.R r, RomLayout.F f, byte[] data) {
        final int chunk = r.chunk, stride = r.stride;
        final int n = data.length / chunk;
        int dst = (int) f.off, src = 0;
        if (chunk == 2 && r.swap) {
            for (int i = 0; i < n; i++, dst += stride, src += 2) { mb.put(dst, data[src + 1]); mb.put(dst + 1, data[src]); }
        } else if (chunk == 2) {
            for (int i = 0; i < n; i++, dst += stride, src += 2) { mb.put(dst, data[src]); mb.put(dst + 1, data[src + 1]); }
        } else if (chunk == 1) {
            for (int i = 0; i < n; i++, dst += stride, src++) mb.put(dst, data[src]);
        } else {
            for (int i = 0; i < n; i++, dst += stride, src += chunk)
                for (int k = 0; k < chunk; k++) mb.put(dst + k, data[src + (r.swap ? chunk - 1 - k : k)]);
        }
    }
}
