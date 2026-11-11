//! Headless core for Omawrite Next.
//!
//! The UI never walks the filesystem or guesses how a wikilink resolves. It
//! asks this crate, which owns the vault, the warm note index, frontmatter and
//! note moves. The design borrows two habits from Helix: a small set of plain
//! data types at the boundary, and one command per real user action rather
//! than a mesh of callbacks.

pub mod frontmatter;
pub mod index;
pub mod links;
pub mod note_ops;
pub mod settings;
pub mod theme;
pub mod vault;

pub use frontmatter::{Frontmatter, Property, PropertyKind};
pub use index::{Backlink, NoteIndex, NoteMeta, SearchHit, Suggestion, TreeNode};
pub use note_ops::{move_note, OperationResult};
pub use settings::Settings;
pub use theme::Theme;
pub use vault::{Vault, VaultInfo, VaultKind};
