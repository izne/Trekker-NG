#pragma once

namespace tui {

// M11: the About box (SPEC §6) - a centered modal listing the app version
// and the third-party libraries with their licenses. pOpen is the shared
// state: the status-bar [about] button sets it, Esc (handled in the event
// loop before ImGui sees it) and the close button clear it, and
// BeginPopupModal's own p_open contract resyncs it when the popup closes
// by any other route.
void drawAbout(bool* pOpen);

} // namespace tui
