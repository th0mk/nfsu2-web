//! The game's files in blocks, compressed: /data-blocks/<path>?b=<first>&n=<count> is blocks first .. first+count-1
//! of a game file (BLOCK bytes each, the last one shorter; BLOCKS_MAX at most) as one 200 answer, compressed with what
//! the client takes (zstd, br or gzip as Content-Encoding, which the browser undoes by itself).  The game data then
//! crosses the network in about 70 % of its size, the tracks in about 40 %.  The page's I/O and predownload workers ask
//! for these when the manifest names them (its "blocks"), and for byte ranges of /data/ otherwise.
//!
//! The compressor streams, so a client that stops reading holds no more than its buffers.  Sent as they are, never
//! queued or refused: a run whose first PROBE bytes do not compress (the movies' VP6 video, most car textures; judged
//! per run, not per file: the music's first megabyte does not compress, the rest does), and an answer while the
//! compressors are taken (per client, per site, in all).  An answer holds its compressor until the client has its
//! last byte, so most of them wait on the network: about 100 players draw some 70 MB/s of game data, dozens of answers
//! under way at once.  The CPU follows the bytes, not the answers (br 3 does 200 MB/s on one core of an M4 Pro), so
//! the total bounds memory: 128 compressors take about 200 MB at most (zstd 1.5 MB each, br 0.9, gzip 0.4).  The
//! compressing itself, chunk by chunk, is on the pool's threads (pool.rs), never on the runtime's workers.
//!
//! A shared cache (Cloudflare's, with a Cache Rule for /data-blocks/*) may keep a run for good (EDGE_KEEP), as the
//! game data is the same for every player: only when its address names this version of the file (m and s, the
//! manifest's mtime and size, so a changed file is a new address) and it is as small as it gets (compressed, or a run
//! that does not compress; not one sent as it is because the compressors were taken).  The movies too.  At about 110
//! players the 1 Gbit uplink was full without it.
//!
//! One block (n=1, all the page asks for) to a client that takes br is better still: the block as kept.rs keeps it,
//! compressed once at brotli's best (or as it is, kept.rs having found it does not compress), with its length.  A
//! block not kept yet is answered as above and queued to be kept; a shared cache may keep that answer EDGE_WHILE
//! only, then asks again and keeps the kept one for good.  The addresses are /data-blocks/v2/ (v2 since blocks are
//! kept, so that no shared cache's copy of an answer from before is ever asked for again).

use std::io::{self, Write};
use std::os::unix::fs::{FileExt, MetadataExt};
use std::sync::Arc;
use std::sync::atomic::{AtomicU64, Ordering};

use nfsu2_headers::{Cache, EDGE_KEEP, pick_encoding};
use nfsu2_http::{Request, Response};

use super::files::{self, stream_range};
use super::kept::{self, Kept};
use super::manifest::{BLOCK, BLOCKS_MAX};
use super::url::Url;
use super::{App, Fail, Res, Share, etag, finish, send};

/// the bytes a run is judged by
pub const PROBE: usize = 64 << 10;
pub const COMPRESSORS_PER_CLIENT: usize = 8;
pub const COMPRESSORS_PER_SITE: usize = 16;
pub const COMPRESSORS_TOTAL: usize = 128;
/// the encoders, in the server's order of preference
pub const ENCODERS: [&str; 3] = ["zstd", "br", "gzip"];
/// what one job reads and compresses
const CHUNK: usize = 128 << 10;
/// a block answered before it is kept (compressed as it is sent, or as it is): a shared cache keeps it 10 minutes
pub const EDGE_WHILE: &str = "public, max-age=600";
/// the addresses' own folder since blocks are kept (the manifest's "blocks")
pub const V2: &str = "/v2";

/// The answers of /data-blocks/ since the last look: kept blocks, runs compressed as they are sent (by encoding:
/// zstd, br, gzip), as they are.  The 5-minute line (run.rs) shows whether a CDN in front asks for br (Cloudflare
/// does, from servers that give it): without it no kept block goes out.
#[derive(Default)]
pub struct Sent {
    kept: AtomicU64,
    compressed: [AtomicU64; 3],
    as_is: AtomicU64,
}

impl Sent {
    fn count(c: &AtomicU64) {
        c.fetch_add(1, Ordering::Relaxed);
    }

    /// the counts since the last look ("" when there were none), set back to 0
    pub fn take(&self) -> String {
        let t = |c: &AtomicU64| c.swap(0, Ordering::Relaxed);
        let (kept, z, as_is) = (t(&self.kept), self.compressed.each_ref().map(t), t(&self.as_is));
        if kept + z.iter().sum::<u64>() + as_is == 0 {
            return String::new();
        }
        format!("{kept} kept, compressed as sent {} zstd {} br {} gzip, {as_is} as they are", z[0], z[1], z[2])
    }
}

/// A compressor's state, which moves with its run's jobs from chunk to chunk.  Fast settings, at a fraction of the
/// CPU of the best (zstd 3: about 260 MB/s on one core; br 3, window 2^21; gzip 4): brotli's best is about 8 %
/// smaller for the game data, a fifth for the tracks, which is why kept.rs keeps the blocks that way.
pub enum Encoder {
    Zstd(zstd::stream::write::Encoder<'static, Vec<u8>>),
    Br(Box<brotli::CompressorWriter<Vec<u8>>>),
    Gzip(flate2::write::GzEncoder<Vec<u8>>),
}

impl Encoder {
    pub fn new(name: &str) -> io::Result<Encoder> {
        Ok(match name {
            "zstd" => Encoder::Zstd(zstd::stream::write::Encoder::new(Vec::new(), 3)?),
            "br" => Encoder::Br(Box::new(brotli::CompressorWriter::new(Vec::new(), 64 << 10, 3, 21))),
            // (the header as zlib writes it on a Unix: OS 3)
            _ => Encoder::Gzip(flate2::GzBuilder::new().operating_system(3).write(Vec::new(), flate2::Compression::new(4))),
        })
    }
    /// data compressed: what has come out so far
    pub fn push(&mut self, data: &[u8]) -> io::Result<Vec<u8>> {
        match self {
            Encoder::Zstd(e) => {
                e.write_all(data)?;
                Ok(std::mem::take(e.get_mut()))
            }
            Encoder::Br(e) => {
                e.write_all(data)?;
                Ok(std::mem::take(e.get_mut()))
            }
            Encoder::Gzip(e) => {
                e.write_all(data)?;
                Ok(std::mem::take(e.get_mut()))
            }
        }
    }
    /// the end of the stream: the rest
    pub fn finish(self) -> io::Result<Vec<u8>> {
        match self {
            Encoder::Zstd(e) => e.finish(),
            Encoder::Br(e) => Ok(e.into_inner()),
            Encoder::Gzip(e) => e.finish(),
        }
    }
}

/// buf filled from the file at off, as far as the file goes
pub fn read_full_at(f: &std::fs::File, buf: &mut [u8], off: u64) -> io::Result<usize> {
    let mut got = 0;
    while got < buf.len() {
        match f.read_at(&mut buf[got..], off + got as u64) {
            Ok(0) => break,
            Ok(n) => got += n,
            Err(e) if e.kind() == io::ErrorKind::Interrupted => {}
            Err(e) => return Err(e),
        }
    }
    Ok(got)
}

/// whether the bytes from start compress, judged from the first PROBE of them (raw deflate 1: about 0.3 ms)
pub fn compresses(f: &std::fs::File, start: u64, end: u64) -> io::Result<bool> {
    let mut buf = vec![0u8; PROBE.min((end - start) as usize)];
    let got = read_full_at(f, &mut buf, start)?;
    // zlib-rs's level 2 is zlib's level 1 (deflate_fast, 4/4/8/4, Huffman trees made for the data); its own level 1
    // is zlib-ng's "quick" (fixed trees only), which judged 1124 of the game folder's 4067 runs (car textures, vinyls)
    // as not compressing where Node's zlib compresses them.  Level 2 judges one of them otherwise (14 bytes from the
    // threshold).
    let mut z = flate2::write::DeflateEncoder::new(Vec::with_capacity(got + 64), flate2::Compression::new(2));
    z.write_all(&buf[..got])?;
    let z = z.finish()?;
    Ok((z.len() as f64) < got as f64 * 0.97)
}

/// one job's work: the run's next chunk read and compressed
struct Step {
    /// None: the stream is finished
    enc: Option<Encoder>,
    out: Vec<u8>,
    off: u64,
}

fn step(file: &std::fs::File, mut enc: Encoder, off: u64, end: u64) -> io::Result<Step> {
    let want = CHUNK.min((end - off) as usize);
    let mut buf = vec![0u8; want];
    let got = read_full_at(file, &mut buf, off)?;
    let mut out = enc.push(&buf[..got])?;
    let off = off + got as u64;
    // the end of the run (or of a file that came up short)
    if off >= end || got < want {
        out.extend(enc.finish()?);
        return Ok(Step { enc: None, out, off });
    }
    Ok(Step { enc: Some(enc), out, off })
}

impl App {
    /// /data-blocks/: the run of the game file `file` that b and n name (None: 404), compressed or as it is
    pub async fn send_blocks(&self, q: &Request, s: &mut Response, file: Option<String>, u: &Url) -> Res {
        // one open file for the whole answer, its size and version, the probe and the bytes: a file put in place
        // meanwhile is another one, and a run kept for good is the version its address names (opened where it is
        // looked for: one trip to a blocking thread)
        let opened = match file {
            Some(file) => {
                let (roots, state, root) = (self.real_roots.clone(), self.real_state.clone(), self.o.data.clone());
                tokio::task::spawn_blocking(move || {
                    let f = files::locate_sync(&roots, &state, &root, &file)?;
                    let (file, md) = files::open_found(&f.real).ok()?;
                    Some((file, md, f.real))
                })
                .await
                .ok()
                .flatten()
            }
            None => None,
        };
        let Some((file, md, real)) = opened else { return self.not_found(q, s).await };
        let (b, n) = (u.param("b").unwrap_or_default(), u.param("n").unwrap_or_default());
        let digits = |x: &str, max: usize| !x.is_empty() && x.len() <= max && x.bytes().all(|c| c.is_ascii_digit());
        if !digits(&b, 7) || !digits(&n, 2) {
            return self.bad_request(q, s).await;
        }
        let (b, n): (u64, u64) = (b.parse().unwrap_or(0), n.parse().unwrap_or(0));
        if !(1..=BLOCKS_MAX).contains(&n) {
            return self.bad_request(q, s).await;
        }
        let file = Arc::new(file);
        let size = md.size();
        let (start, end) = (b * BLOCK, size.min((b + n) * BLOCK));
        if start >= size {
            return self.plain(q, s, 416, "Range not satisfiable", &[], Cache::Store, true).await;
        }
        // the version: m and s
        let named = u.param("m").as_deref() == Some(etag::num(etag::mtime_s(&md)).as_str())
            && u.param("s").as_deref() == Some(etag::num(size as f64).as_str());
        let accept = q.headers.get("accept-encoding");
        // one block to a client that takes br: as kept.rs keeps it, if it does yet
        let kept = match &self.kept {
            Some(k) if n == 1 && pick_encoding(accept, &["br"]).is_some() => Some(k.clone()),
            _ => None,
        };
        let found = match &kept {
            Some(k) => {
                let (k, key) = (k.clone(), kept::key(&real, &md, b));
                tokio::task::spawn_blocking(move || k.lookup(&key)).await.unwrap_or(Kept::No)
            }
            None => Kept::No,
        };
        let found = match found {
            Kept::Br(f, len) => return self.send_kept(q, s, f, len, named).await,
            other => other,
        };
        let (_, key, site) = self.client(q);
        let enc = pick_encoding(accept, &ENCODERS);
        let mut held = match enc {
            Some(_) if q.method != "HEAD" && !matches!(found, Kept::AsIs) => Share::take(&self.compressors, &key, &site),
            _ => None,
        };
        // a run whose probe failed is compressed: never kept as it is
        let as_is = matches!(found, Kept::AsIs)
            || held.is_some() && {
                let f = file.clone();
                !self.pool.submit(move || compresses(&f, start, end).unwrap_or(true)).await.unwrap_or(true)
            };
        if as_is {
            held = None;
        }
        let z = held.is_some();
        // a block that is not kept yet: queued, and its answer now (compressed as it is sent, or as it is where the
        // probe says so) kept by a shared cache for a while only
        let mut kept_soon = false;
        if let Some(k) = &kept
            && matches!(found, Kept::No)
            && k.keeping()
        {
            kept_soon = true;
            if q.method != "HEAD" {
                k.queue(&real, &md, b);
            }
        }
        let keep = (z || as_is) && named;
        let mut h = self.headers(q, Cache::Store, true);
        h.set("content-type", "application/octet-stream");
        h.set("vary", "accept-encoding");
        if keep {
            let v = if kept_soon { EDGE_WHILE } else { EDGE_KEEP };
            h.set("cdn-cache-control", v);
            h.set("cloudflare-cdn-cache-control", v);
        }
        match enc {
            Some(e) if z => {
                h.set("content-encoding", e);
                Sent::count(&self.sent.compressed[ENCODERS.iter().position(|x| *x == e).unwrap_or(0)]);
            }
            _ => {
                h.set("content-length", (end - start).to_string());
                Sent::count(&self.sent.as_is);
            }
        }
        s.head(200, h.0);
        if q.method == "HEAD" {
            return finish(s).await;
        }
        match enc {
            Some(e) if z => self.stream_compressed(s, file, e, start, end).await?,
            _ => stream_range(s, file, start, end).await?,
        }
        // the compressor is given back once the client has the last byte
        let r = finish(s).await;
        drop(held);
        r
    }

    /// a kept block: its brotli stream as it is on the disk, with its length (kept for good by a shared cache when
    /// the address names this version)
    async fn send_kept(&self, q: &Request, s: &mut Response, f: std::fs::File, len: u64, named: bool) -> Res {
        let mut h = self.headers(q, Cache::Store, true);
        h.set("content-type", "application/octet-stream");
        h.set("vary", "accept-encoding");
        if named {
            h.set("cdn-cache-control", EDGE_KEEP);
            h.set("cloudflare-cdn-cache-control", EDGE_KEEP);
        }
        h.set("content-encoding", "br");
        h.set("content-length", len.to_string());
        Sent::count(&self.sent.kept);
        s.head(200, h.0);
        if q.method != "HEAD" {
            stream_range(s, Arc::new(f), 0, len).await?;
        }
        finish(s).await
    }

    /// the run compressed chunk by chunk on the pool (the encoder made there too), the next chunk compressing while
    /// this one goes out
    async fn stream_compressed(&self, s: &mut Response, file: Arc<std::fs::File>, name: &'static str, start: u64, end: u64) -> Res {
        let job = |enc: Option<Encoder>, off: u64| {
            let f = file.clone();
            self.pool.submit(move || {
                let enc = match enc {
                    Some(e) => e,
                    None => Encoder::new(name)?,
                };
                step(&f, enc, off, end)
            })
        };
        let mut next = Some(job(None, start));
        while let Some(j) = next.take() {
            let Some(Ok(st)) = j.await else {
                // a read or compression error: the answer cannot be finished
                s.destroy();
                return Err(Fail::Gone);
            };
            if let Some(enc) = st.enc {
                next = Some(job(Some(enc), st.off));
            }
            if !st.out.is_empty() {
                send(s, &st.out).await?;
            }
        }
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::io::Read;

    fn data(n: usize, compressible: bool) -> Vec<u8> {
        let mut x = 0x1234_5678_9abc_def0u64;
        (0..n)
            .map(|_| {
                x ^= x << 13;
                x ^= x >> 7;
                x ^= x << 17;
                if compressible { ((x & 3) as u8) * 0x41 } else { x as u8 }
            })
            .collect()
    }

    fn decode(name: &str, z: &[u8]) -> Vec<u8> {
        let mut out = Vec::new();
        match name {
            "zstd" => out = zstd::stream::decode_all(z).unwrap(),
            "br" => {
                brotli::Decompressor::new(z, 4096).read_to_end(&mut out).unwrap();
            }
            _ => {
                flate2::read::GzDecoder::new(z).read_to_end(&mut out).unwrap();
            }
        }
        out
    }

    // the encoders: the exact bytes back, chunk by chunk, and their parameters
    #[test]
    fn every_encoder_gives_the_bytes_back() {
        let raw = data(3 * CHUNK + 17, true);
        for name in ENCODERS {
            let mut e = Encoder::new(name).unwrap();
            let mut z = Vec::new();
            for c in raw.chunks(CHUNK) {
                z.extend(e.push(c).unwrap());
            }
            z.extend(e.finish().unwrap());
            assert_eq!(decode(name, &z), raw, "{name}");
            assert!(z.len() < raw.len() / 2, "{name}: {} for {}", z.len(), raw.len());
            match name {
                "zstd" => assert_eq!(&z[..4], &[0x28, 0xb5, 0x2f, 0xfd]),
                // WBITS 21: the stream header's first bits 1, then 21 - 17 = 4
                "br" => assert_eq!(z[0] & 0x0f, 1 | (4 << 1)),
                // gzip: deflate, no flags, mtime 0, XFL 0 (level 4), OS 3
                _ => assert_eq!(&z[..10], &[0x1f, 0x8b, 8, 0, 0, 0, 0, 0, 0, 3]),
            }
        }
    }

    /// a measurement, not a check: each encoder over the first 32 MiB of $NFSU2_BENCH_FILE, chunk by chunk as an
    /// answer compresses it (cargo test --release -- --ignored bench_encoders --nocapture)
    #[test]
    #[ignore]
    fn bench_encoders() {
        let Ok(path) = std::env::var("NFSU2_BENCH_FILE") else { return };
        let mut raw = std::fs::read(&path).unwrap();
        raw.truncate(32 << 20);
        for name in ENCODERS {
            let t0 = std::time::Instant::now();
            let mut e = Encoder::new(name).unwrap();
            let mut n = 0;
            for c in raw.chunks(CHUNK) {
                n += e.push(c).unwrap().len();
            }
            n += e.finish().unwrap().len();
            let s = t0.elapsed().as_secs_f64();
            println!(
                "{name}: {:.1} MB -> {:.1} MB ({:.0} %) in {:.0} ms, {:.0} MB/s",
                raw.len() as f64 / 1e6,
                n as f64 / 1e6,
                100.0 * n as f64 / raw.len() as f64,
                s * 1e3,
                raw.len() as f64 / 1e6 / s
            );
        }
    }

    #[test]
    fn the_probe_judges_a_run_by_its_start() {
        let d = tempfile::tempdir().unwrap();
        let p = d.path().join("f");
        let mut b = data(1 << 20, false);
        b.extend(data(1 << 20, true));
        std::fs::write(&p, &b).unwrap();
        let f = std::fs::File::open(&p).unwrap();
        assert!(!compresses(&f, 0, 1 << 20).unwrap(), "random bytes");
        assert!(compresses(&f, 1 << 20, 2 << 20).unwrap(), "bytes of 2 bits");
        assert!(!compresses(&f, 4 << 20, 5 << 20).unwrap(), "past the end: nothing read");
    }

    #[test]
    fn steps_finish_the_stream() {
        let d = tempfile::tempdir().unwrap();
        let p = d.path().join("f");
        let raw = data(CHUNK * 2 + 5, true);
        std::fs::write(&p, &raw).unwrap();
        let f = std::fs::File::open(&p).unwrap();
        let mut z = Vec::new();
        let mut st = step(&f, Encoder::new("gzip").unwrap(), 0, raw.len() as u64).unwrap();
        z.extend(std::mem::take(&mut st.out));
        while let Some(enc) = st.enc.take() {
            st = step(&f, enc, st.off, raw.len() as u64).unwrap();
            z.extend(std::mem::take(&mut st.out));
        }
        assert_eq!(st.off, raw.len() as u64);
        assert_eq!(decode("gzip", &z), raw);
        // a file shorter than the run: the stream ends where the file does
        let mut st = step(&f, Encoder::new("zstd").unwrap(), CHUNK as u64 * 2, CHUNK as u64 * 3).unwrap();
        assert!(st.enc.is_none());
        assert_eq!(decode("zstd", &std::mem::take(&mut st.out)), raw[CHUNK * 2..]);
    }
}
