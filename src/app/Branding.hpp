#pragma once

#include <QString>

namespace cad::app {

/// What the application is called, in one place.
///
/// The product is **Slate**. "Slate CAD" is its full name, and the "CAD" is
/// there so that somebody looking for drafting software finds it: a person who
/// has never heard of us searches for what they want to do, not for a name they
/// do not know yet.
///
/// So the two are used in different places on purpose, and neither is a
/// shortening of the other:
///
///  - `kProductName` — "Slate CAD" — is the name of the *thing you install*. It
///    goes on the executable, the installer, the Start menu entry and anywhere
///    a search engine or a file index will read it.
///  - `kDisplayName` — "Slate" — is what the application calls itself once it is
///    open and the finding is done. A title bar that repeats a search keyword
///    at its user every time they look at it is an advertisement, not an
///    interface.
inline constexpr const char* kProductName = "Slate CAD";
inline constexpr const char* kDisplayName = "Slate";

/// Lower-case, no spaces: configuration directories, temporary files, and
/// anything else where the name has to survive a file system.
inline constexpr const char* kShortName = "slate";

/// What the product *does*, in the words somebody would search for.
///
/// This is the description the executable carries and the installer registers,
/// and it is deliberately made of the terms a person types when they are
/// looking for this and do not know it exists: the file formats, the task, and
/// the incumbent they are hoping to open files from.
///
/// It says DWG compatible, which is what this is and all it claims to be.
/// The compatibility is the honest reason the word belongs here, and stating it
/// that way rather than as a bare keyword is both more useful to a reader and
/// the form that does not pretend to be somebody else's product.
inline constexpr const char* kDescription =
    "Slate CAD - 2D CAD drafting, DWG compatible, opens and saves DWG and DXF";

/// The previous name, kept only so that a user who had it installed does not
/// lose their settings and their unsaved work on the day it was renamed.
inline constexpr const char* kPreviousProductName = "ApertureCAD";

}  // namespace cad::app
