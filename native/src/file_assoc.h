#pragma once

// Register / unregister HKCU file associations so text/code files offer
// DeskNote in the "Open with" list (reversible; never touches HKLM).
bool RegisterNoteFileAssociations();
bool UnregisterNoteFileAssociations();
