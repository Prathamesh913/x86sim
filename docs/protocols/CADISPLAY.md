# Why [CADisplay mainDisplay] is nil in CA clients (QuartzCore iOS 26.5 x86_64)

Client path: +[CADisplay displays]/mainDisplay -> ensure_displays (0x19162) -> query_displays (0x19393)
 - CARenderServerGetServerPort(0) (com.apple.CARenderServer) -> MIG _CASGetDisplays -> plist array of dicts
   {kCADisplayName, kCADisplayDeviceName, kCADisplayId}. No SFB, no Metal, no getenv on client side.
 - port==0: retry every 0.5s until a timeout, then return empty (cached via displays_up_to_date).
 - MIG error other than MIG_SERVER_DIED: log "Unable to query displays from server" + abort.
 - Main display selection (0x19733..0x19760): first display whose kCADisplayName == @"LCD".
   Else _tvoutDisplay (name "TVOut" or "External") becomes main (0x19859). Else nil
   (internal-build-only fallback at 0x1988a).
Server: CA::WindowServer::Server::get_displays (0x2859ae): kCADisplayName = Server name,
 which for SimDisplay = SFBDisplayGetName() (set at 0x25c371 in -[CAWindowServer _detectSimDisplays]).
FIX: SFBDisplayGetName must return "LCD" for the main display (UID can stay whatever).
