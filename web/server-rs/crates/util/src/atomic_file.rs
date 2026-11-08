//! A file written whole in place of the old one: a temporary file beside it, then a rename over it, so that a reader
//! (and the next start after a crash) finds the old file or the new one, never part of one.
//!
//! [`write`] does it the safe way: the temporary file is named for this process and this write alone
//! (`<name>.tmp-<pid>-<n>`), made new with mode 0600 (a crash's leftover under the same name is removed first: in a
//! container the server is pid 1 at every start), synced to the disk before the rename (else a power cut can leave
//! the new name on a file cut short), and removed when a step fails.  [`Options`] changes only what a caller needs
//! otherwise: the folder made when it is missing, no sync for a writer on a busy thread whose file is no great loss,
//! a temporary name that starts with a "." for a folder whose sweep takes every other name for its own.

use std::ffi::OsString;
use std::fmt;
use std::fs::{self, File};
use std::io::{self, Write};
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicU64, Ordering};

/// the writes of this process: each temporary name is its own
static SEQ: AtomicU64 = AtomicU64::new(0);

/// `body` written whole in place of `file`, the safe way (see the module's text)
pub fn write(file: &Path, body: &[u8]) -> io::Result<()> {
    Options::new().write(file, body).map_err(io::Error::from)
}

/// How a write goes: [`Options::new`] is the safe way, the methods change one thing each.
#[derive(Clone, Copy, Debug)]
pub struct Options {
    folder: bool,
    sync: bool,
    hidden: bool,
}

impl Default for Options {
    fn default() -> Self {
        Self::new()
    }
}

impl Options {
    /// the folder must be there, the file is synced, the temporary name is `<name>.tmp-<pid>-<n>`
    pub const fn new() -> Self {
        Options { folder: false, sync: true, hidden: false }
    }

    /// the folder made when it is missing (0700, with its parents)
    pub const fn make_folder(mut self) -> Self {
        self.folder = true;
        self
    }

    /// renamed without a sync: a power cut may leave the file cut short or empty, so only for a file that is no great
    /// loss, written often from a thread that must not wait on the disk
    pub const fn no_sync(mut self) -> Self {
        self.sync = false;
        self
    }

    /// the temporary name starts with a "." (`.<name>.tmp-<pid>-<n>`)
    pub const fn hidden(mut self) -> Self {
        self.hidden = true;
        self
    }

    /// `body` written whole in place of `file`
    pub fn write(self, file: &Path, body: &[u8]) -> Result<(), Failed> {
        self.write_with(file, |f| f.write_all(body))
    }

    /// what `fill` writes into the temporary file, in place of `file`
    pub fn write_with(self, file: &Path, fill: impl FnOnce(&mut File) -> io::Result<()>) -> Result<(), Failed> {
        if self.folder
            && let Some(dir) = file.parent()
        {
            let mut b = fs::DirBuilder::new();
            b.recursive(true);
            #[cfg(unix)]
            {
                use std::os::unix::fs::DirBuilderExt;
                b.mode(0o700);
            }
            b.create(dir).map_err(|error| Failed { step: Step::Folder, path: dir.to_path_buf(), tmp: PathBuf::new(), error })?;
        }
        let tmp = temp_name(file, self.hidden);
        let mut f = match open_new(&tmp) {
            Ok(f) => f,
            Err(error) => return Err(Failed { step: Step::Open, path: tmp.clone(), tmp, error }),
        };
        let done = fill(&mut f)
            .map_err(|e| (Step::Write, e))
            .and_then(|()| if self.sync { f.sync_all().map_err(|e| (Step::Sync, e)) } else { Ok(()) });
        drop(f);
        let done = done.and_then(|()| fs::rename(&tmp, file).map_err(|e| (Step::Rename, e)));
        match done {
            Ok(()) => Ok(()),
            Err((step, error)) => {
                let _ = fs::remove_file(&tmp);
                let path = if step == Step::Rename { file.to_path_buf() } else { tmp.clone() };
                Err(Failed { step, path, tmp, error })
            }
        }
    }
}

/// `<name>.tmp-<pid>-<n>` beside `file` (a "." in front when `hidden`)
fn temp_name(file: &Path, hidden: bool) -> PathBuf {
    let mut name = OsString::new();
    if hidden {
        name.push(".");
    }
    name.push(file.file_name().unwrap_or_default());
    name.push(format!(".tmp-{}-{}", std::process::id(), SEQ.fetch_add(1, Ordering::Relaxed)));
    file.with_file_name(name)
}

/// a new file of mode 0600 (never one that is there: O_EXCL follows no link); a crash's leftover of the same name goes
fn open_new(tmp: &Path) -> io::Result<File> {
    let open = || {
        let mut o = fs::OpenOptions::new();
        o.write(true).create_new(true);
        #[cfg(unix)]
        {
            use std::os::unix::fs::OpenOptionsExt;
            o.mode(0o600);
        }
        o.open(tmp)
    };
    match open() {
        Err(e) if e.kind() == io::ErrorKind::AlreadyExists => {
            let _ = fs::remove_file(tmp);
            open()
        }
        r => r,
    }
}

/// the step of a write that failed
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Step {
    /// making the folder ([`Options::make_folder`])
    Folder,
    /// making the temporary file
    Open,
    /// writing it
    Write,
    /// syncing it
    Sync,
    /// renaming it over the file
    Rename,
}

/// A write that failed: at which step, on what (the folder, the temporary file, or for the rename the file), and why.
/// Shown as "<path>: <error>"; as an `io::Error`, the error alone.
#[derive(Debug)]
pub struct Failed {
    pub step: Step,
    pub path: PathBuf,
    /// the temporary file (empty when the folder could not be made)
    pub tmp: PathBuf,
    pub error: io::Error,
}

impl fmt::Display for Failed {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "{}: {}", self.path.display(), self.error)
    }
}

impl std::error::Error for Failed {
    fn source(&self) -> Option<&(dyn std::error::Error + 'static)> {
        Some(&self.error)
    }
}

impl From<Failed> for io::Error {
    fn from(f: Failed) -> io::Error {
        f.error
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn temp(name: &str) -> PathBuf {
        let d = std::env::temp_dir().join(format!("nfsu2-atomic-{name}-{}", std::process::id()));
        let _ = fs::remove_dir_all(&d);
        fs::create_dir_all(&d).unwrap();
        d
    }

    fn names(d: &Path) -> Vec<String> {
        let mut v: Vec<String> = fs::read_dir(d).unwrap().map(|e| e.unwrap().file_name().into_string().unwrap()).collect();
        v.sort();
        v
    }

    #[test]
    fn the_file_is_replaced_whole_and_nothing_else_is_left() {
        let d = temp("whole");
        let f = d.join("x.json");
        write(&f, b"one").unwrap();
        write(&f, b"two").unwrap();
        assert_eq!(fs::read(&f).unwrap(), b"two");
        assert_eq!(names(&d), ["x.json"]);
        #[cfg(unix)]
        {
            use std::os::unix::fs::PermissionsExt;
            assert_eq!(fs::metadata(&f).unwrap().permissions().mode() & 0o777, 0o600);
        }
        let _ = fs::remove_dir_all(&d);
    }

    #[test]
    fn temporary_names_are_this_processs_and_each_writes_own() {
        let f = Path::new("/s/x.json");
        let (a, b) = (temp_name(f, false), temp_name(f, false));
        assert_ne!(a, b);
        let prefix = format!("x.json.tmp-{}-", std::process::id());
        assert!(a.file_name().unwrap().to_str().unwrap().starts_with(&prefix), "{a:?}");
        assert_eq!(a.parent(), Some(Path::new("/s")));
        assert!(temp_name(f, true).file_name().unwrap().to_str().unwrap().starts_with(&format!(".{prefix}")));
    }

    #[test]
    fn a_crashs_leftover_of_the_same_name_is_written_over() {
        let d = temp("leftover");
        let tmp = d.join("t");
        fs::write(&tmp, b"half of something").unwrap();
        let mut f = open_new(&tmp).unwrap();
        f.write_all(b"new").unwrap();
        drop(f);
        assert_eq!(fs::read(&tmp).unwrap(), b"new");
        let _ = fs::remove_dir_all(&d);
    }

    #[test]
    fn the_folder_is_made_only_when_asked() {
        let d = temp("folder");
        let f = d.join("a/b/x.json");
        let e = Options::new().write(&f, b"x").unwrap_err();
        assert_eq!(e.step, Step::Open);
        assert_eq!(e.path, e.tmp);
        Options::new().make_folder().no_sync().write(&f, b"x").unwrap();
        assert_eq!(fs::read(&f).unwrap(), b"x");
        #[cfg(unix)]
        {
            use std::os::unix::fs::PermissionsExt;
            assert_eq!(fs::metadata(d.join("a/b")).unwrap().permissions().mode() & 0o777, 0o700);
        }
        let _ = fs::remove_dir_all(&d);
    }

    #[test]
    fn a_failed_step_leaves_no_temporary_file() {
        let d = temp("failed");
        // the fill fails
        let e = Options::new().write_with(&d.join("x"), |_| Err(io::Error::other("no"))).unwrap_err();
        assert_eq!((e.step, e.error.to_string()), (Step::Write, "no".to_string()));
        assert_eq!(e.to_string(), format!("{}: no", e.tmp.display()));
        // the rename fails: a folder is in the file's place
        fs::create_dir(d.join("y")).unwrap();
        fs::write(d.join("y/z"), b"").unwrap();
        let e = Options::new().write(&d.join("y"), b"x").unwrap_err();
        assert_eq!((e.step, e.path.clone()), (Step::Rename, d.join("y")));
        assert_eq!(names(&d), ["y"]);
        let _ = fs::remove_dir_all(&d);
    }
}
