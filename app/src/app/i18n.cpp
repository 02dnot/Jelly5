/*
 * Jelly5 — Jellyfin for PS5
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "app/i18n.h"
#include "app/i18n_tables.h"

#include "evo_boot_trace.h"

#include <atomic>
#include <mutex>
#include <set>
#include <unordered_map>

extern "C" int sceSystemServiceParamGetInt(int param_id, int *value);

namespace i18n {
namespace {

std::atomic<int> s_lang{(int)Lang::Norwegian};
std::atomic<unsigned> s_gen{0};

constexpr int kParamLang = 1;           /* SCE_SYSTEM_SERVICE_PARAM_ID_LANG */
constexpr int kSystemNorwegian = 15;    /* SCE_SYSTEM_PARAM_LANG_NORWEGIAN */

/* SCE_SYSTEM_PARAM_LANG_* -> the language Jelly5 has for it. */
Lang from_system(int sys)
{
    switch (sys) {
    case 15: return Lang::Norwegian;
    case 3: case 20: return Lang::Spanish;          /* Spain, Latin America */
    case 2: case 22: return Lang::French;           /* France, Canada */
    case 4: return Lang::German;
    case 7: case 17: return Lang::Portuguese;       /* Portugal, Brazil */
    case 5: return Lang::Italian;
    default: return Lang::English;
    }
}

/* Norwegian -> English. Texts that read the same in both are not listed. */
const std::unordered_map<std::string, const char *> &english_table()
{
    static const std::unordered_map<std::string, const char *> t = {
        /* tabs, rows, home */
        {"Hjem", "Home"}, {"Musikk", "Music"}, {"Album", "Albums"}, {"Artister", "Artists"},
        {"Spillelister", "Playlists"}, {"Filmer", "Movies"}, {"Serier", "TV Shows"}, {"Søk", "Search"},
        {"Innstillinger", "Settings"}, {"Henter biblioteket …", "Loading your library …"},
        {"Nylig lagt til i ", "Recently added in "}, {"Fortsett å se", "Continue Watching"},
        {"Neste episode", "Next Episode"}, {"Min liste", "My List"}, {"Biblioteker", "Libraries"},
        {"Fordi du så ", "Because you watched "}, {"Fordi du likte ", "Because you liked "},
        {"Regissert av ", "Directed by "}, {"Med ", "Starring "}, {"Mer info", "More Info"},
        {"Spill av", "Play"}, {"Fortsett", "Resume"}, {"Ingenting å vise ennå", "Nothing to show yet"},
        {"Legg til filmer eller serier på serveren din.", "Add movies or shows on your server."},
        /* connecting, errors */
        {"Kobler til ", "Connecting to "}, {"Kobler til …", "Connecting …"},
        {"Får ikke kontakt med ", "Can't reach "}, {" – prøver igjen …", " – trying again …"},
        {"○ bytt bruker eller server", "○ change user or server"},
        {"Jelly5: serveren fant ingen miks her", "Jelly5: the server found no mix here"},
        {"Jelly5: fant ingenting å spille av her", "Jelly5: nothing to play here"},
        {"Jelly5: kunne ikke spille av\n", "Jelly5: could not play\n"},
        {"Jelly5: fant ikke det som ble sendt", "Jelly5: couldn't find what was sent"},
        {"Jelly5: skjermen kunne ikke startes", "Jelly5: the display could not start"},
        /* durations, playback methods */
        {"%d t %d min", "%d h %d min"}, {"Direktespilling", "Direct play"}, {"Direktestrøm", "Direct stream"},
        {"Transkodet av serveren", "Transcoded by the server"},
        /* the player's own strings (Nuvio keys) */
        {"Avansert", "Advanced"}, {"Stil og timing", "Style and timing"},
        {"Forsinkelse, størrelse, posisjon …", "Delay, size, position …"}, {"Lyd", "Audio"},
        {"Bakgrunn", "Background"}, {"Fet skrift", "Bold"}, {"Innebygd", "Built-in"}, {"Standard", "Default"},
        {"Forsinkelse", "Delay"}, {"Slutter kl. %1$s", "Ends at %1$s"}, {"Tvungen", "Forced"},
        {"Tilbake", "Back"}, {"Språk", "Language"}, {"Laster …", "Loading …"}, {"Spilles om %1$s", "Plays in %1$s"},
        {"Ingen andre lydspor", "No other audio tracks"},
        {"Ingen undertekster for denne strømmen", "No subtitles for this stream"}, {"Av", "Off"}, {"På", "On"},
        {"Kontur", "Outline"}, {"Avspillingsfeil", "Playback error"}, {"Spiller", "Playing"},
        {"Posisjon", "Position"}, {"Trykk ✕ for å spille", "Press ✕ to play"}, {"Sesong", "Season"},
        {"Størrelse", "Size"}, {"Hopp over intro", "Skip Intro"}, {"Hopp over forhåndsvisning", "Skip Preview"},
        {"Hopp over oppsummering", "Skip Recap"}, {"Kilder", "Sources"}, {"Spesialer", "Specials"},
        {"Undertekster er av", "Subtitles are off"}, {"Undertekster", "Subtitles"}, {"Spor", "Track"},
        {"Utilgjengelig", "Unavailable"}, {"Ukjent", "Unknown"}, {"Kommer", "Upcoming"},
        {"Du ser på", "You're watching"}, {"Kilde", "Source"},
        /* player interface */
        {"Ukjent språk", "Unknown language"}, {"Undertekst lagt til", "Subtitle added"},
        {"Kunne ikke hente underteksten", "Couldn't get the subtitle"}, {"Tilpass", "Customize"},
        {"Tilpass undertekster", "Customize subtitles"}, {"Tilpass undertekster ›", "Customize subtitles ›"},
        {"Søk etter undertekster", "Search for subtitles"}, {"Søk etter undertekster ›", "Search for subtitles ›"},
        {"Henter undertekst …", "Getting subtitle …"}, {"Slutter kl. ", "Ends at "}, {"Episoder", "Episodes"},
        {"Lyd og undertekster", "Audio & Subtitles"}, {"NESTE EPISODE", "NEXT EPISODE"},
        {"Spilles om %d s  ·  ✕ nå", "Plays in %d s  ·  ✕ now"},
        {"✕ spill av  ·  ○ se rulletekst", "✕ play  ·  ○ watch credits"}, {"Bilde ", "Image "},
        {"Ekstern", "External"}, {"‹ Av ›", "‹ Off ›"}, {"Passer ", "Matches "}, {" nedl.", " downloads"},
        {"Søker …", "Searching …"}, {"Fant ingen", "Found none"}, {"○ lukk", "○ close"},
        {"Sesong %d", "Season %d"}, {"SPILLER NÅ", "NOW PLAYING"}, {"Ingen beskrivelse.", "No description."},
        {"Ingen episoder i denne sesongen.", "No episodes in this season."},
        {"✕ spill av   ·   ○ lukk", "✕ play   ·   ○ close"}, {"Kunne ikke spille av", "Couldn't play"},
        {"Satt på pause", "Paused"}, {"Spilles nå", "Now Playing"}, {"Neste: ", "Next: "}, {"Tvungen ", "Forced "},
        /* detail, person, album */
        {" sesong", " season"}, {" sesonger", " seasons"}, {" min igjen", " min left"}, {"Sett", "Watched"},
        {"Merk som sett", "Mark as Watched"}, {"Fra start", "From the Start"}, {"Med:", "Starring:"},
        {"Regi:", "Director:"}, {"Kanal:", "Network:"}, {"Ekstramateriale", "Extras"},
        {"Skuespillere og crew", "Cast & Crew"}, {"I denne samlingen", "In This Collection"},
        {"Mer som dette", "More Like This"}, {"Født ", "Born "}, {" tittel her", " title here"},
        {" titler her", " titles here"}, {"Ingen biografi.", "No biography."}, {"Ingen titler med ", "No titles with "},
        {" i biblioteket.", " in the library."}, {"Spilleliste", "Playlist"}, {"titler", "titles"},
        {"spor", "tracks"}, {"Bland", "Shuffle"}, {"Miks", "Mix"},
        /* options sheet */
        {"Fjern fra Min liste", "Remove from My List"}, {"Legg til i Min liste", "Add to My List"},
        {"Merk som usett", "Mark as Unwatched"}, {"Fjern fra Fortsett å se", "Remove from Continue Watching"},
        /* libraries, search */
        {"Nylig lagt til", "Recently Added"}, {"A–Å", "A–Z"}, {"Utgivelsesår", "Release Year"},
        {"Vurdering", "Rating"}, {"%d titler", "%d titles"}, {"Henter …", "Loading …"},
        {"Ingenting her ennå", "Nothing here yet"}, {"Filmer, serier, personer, musikk", "Movies, shows, people, music"},
        {"mellomrom", "space"}, {"⌫ slett", "⌫ delete"}, {"▢ sletter", "▢ deletes"}, {"Forslag", "Suggestions"},
        {"Treff for «%s»", "Results for “%s”"}, {"Ingen treff", "No results"}, {"Smart", "Smart"},
        /* sign-in, profiles */
        {"Fant ingen Jellyfin- eller Emby-server på ", "No Jellyfin or Emby server found at "},
        {"Feil brukernavn eller passord", "Wrong username or password"}, {"Innloggingen mislyktes", "Sign-in failed"},
        {"Quick Connect er ikke slått på på denne serveren", "Quick Connect is not enabled on this server"},
        {"Serveradresse", "Server address"}, {"Brukernavn", "Username"}, {"Passord", "Password"},
        {"Koble til Jellyfin eller Emby", "Connect to Jellyfin or Emby"},
        {"Skriv inn adressen til Jellyfin- eller Emby-serveren din, for eksempel 192.168.0.10:8096.", "Enter your Jellyfin or Emby server's address, for example 192.168.0.10:8096."},
        {"Logg inn", "Sign In"}, {"Logger inn …", "Signing in …"}, {"Bruk Quick Connect", "Use Quick Connect"},
        {"Annen server", "Other Server"},
        {"Åpne Jellyfin på telefonen eller PC-en, gå til Innstillinger → Quick Connect og skriv inn koden:",
         "Open Jellyfin on your phone or computer, go to Settings → Quick Connect and enter the code:"},
        {"○ avbryter", "○ cancels"}, {"Legg til", "Add"}, {"Hvem ser på?", "Who's watching?"},
        {"+ Server", "+ Server"},
        {"Trykk △ igjen for å fjerne kontoen fra denne PS5-en", "Press △ again to remove the account from this PS5"},
        {"✕ velg   ·   △ fjern konto", "✕ choose   ·   △ remove account"},
        /* settings */
        {"Ingen preferanse", "No preference"}, {"Alltid", "Always"}, {"Bare tvungne", "Forced only"},
        {"Åpne", "Open"}, {"Skann med telefonen", "Scan with your phone"},
        {"og trykk Godkjenn i Jellyfin", "and tap Authorize in Jellyfin"}, {"Logg inn med brukernavn og passord", "Sign in with username and password"}, {"Versjon", "Version"}, {"Avbryt", "Cancel"}, {"Velg", "Choose"}, {"Fjern konto", "Remove account"},
        {"Trekk tilbake forespørselen", "Cancel request"}, {"Trykk igjen for å trekke tilbake", "Press again to withdraw"}, {"Trekker tilbake …", "Cancelling …"}, {"Forespørselen er trukket tilbake", "The request was cancelled"}, {"Kunne ikke trekke tilbake forespørselen", "Could not cancel the request"}, {"%d av %d forespørsler brukt", "%d of %d requests used"}, {"%d av %d sesonger brukt", "%d of %d seasons used"}, {"%d av %d sesonger brukt (siste %d dager)", "%d of %d seasons used (last %d days)"}, {"Avslått", "Declined"}, {"Feilet", "Failed"}, {"Ingenting å vise akkurat nå – ✕ for å prøve igjen", "Nothing to show right now – ✕ to try again"}, {"Kunne ikke sende forespørselen", "Could not send the request"}, {"Kvoten din er brukt opp", "Your quota is used up"}, {"Kvoten din gir plass til %d sesonger til", "Your quota has room for %d more seasons"}, {"Seerr svarer ikke – ✕ for å prøve igjen", "Seerr is not responding – ✕ to try again"}, {"Ikke pålogget Seerr – ✕ for å logge på igjen", "Not signed in to Seerr – ✕ to sign in again"}, {"Slettet", "Deleted"}, {"Trykk ✕ igjen for å logge ut", "Press ✕ again to sign out"}, {"Trykk ✕ igjen for å trekke tilbake forespørselen", "Press ✕ again to cancel the request"}, {"Valg", "Options"}, {"Seerr tillater ikke denne påloggingen – velg en annen", "Seerr does not allow this sign-in – choose another"}, {"Brukeren finnes ikke i Seerr – be administratoren importere den", "This user is not in Seerr – ask the administrator to import it"}, {"Anbefalt", "Recommended"}, {"Lignende", "Similar"}, {"Nå", "Now"}, {"Kø", "Queue"}, {"Stopp", "Stop"}, {"Gjenta", "Repeat"}, {"Gjenta én", "Repeat one"}, {"Gjenta alle", "Repeat all"}, {"Starter forfra etter denne", "Starts over after this"}, {"Ingenting mer i køen", "Nothing more in the queue"}, {"Nattmodus", "Night mode"}, {"Temamusikk", "Theme music"}, {"Se etter oppdateringer", "Check for updates"},
        {"Jelly5 %s er tilgjengelig – se GitHub", "Jelly5 %s is available – see GitHub"}, {"Jelly5: 3D-filer støttes ikke på PS5", "Jelly5: 3D files aren't supported on PS5"}, {"Spill herfra", "Play from here"}, {"Sorter etter", "Sort by"}, {"1 filter", "1 filter"}, {" filtre", " filters"}, {"Kapitler", "Chapters"}, {"Kapittel ", "Chapter "}, {"Ingen kontakt med serveren – prøver igjen …", "Can't reach the server – trying again …"},
        {"Tilkoblet igjen", "Connected again"}, {"Mistet kontakten med serveren – prøver igjen …", "Lost the connection to the server – trying again …"},
        {"Fikk ikke kontakt med serveren igjen.", "Couldn't reach the server again."}, {"FUNNET PÅ NETTVERKET", "FOUND ON YOUR NETWORK"},
        {"SØKER PÅ NETTVERKET …", "SEARCHING YOUR NETWORK …"}, {"Merk sesongen som sett", "Mark season as watched"},
        {"Merk sesongen som usett", "Mark season as unwatched"}, {"Merk hele serien som sett", "Mark series as watched"},
        {"Merk hele serien som usett", "Mark series as unwatched"}, {"Filtrer", "Filter"}, {"Filter", "Filter"}, {"Bare usette", "Unwatched only"},
        {"Bare favoritter", "Favourites only"}, {"Sjanger", "Genre"}, {"Alle", "All"}, {"Tiår", "Decade"},
        {"%d-tallet", "%ds"}, {"Nullstill filtre", "Clear filters"}, {"Endre", "Change"}, {"Ferdig", "Done"},
        {"Hopp til bokstav", "Jump to letter"}, {"Ingen titler passer filteret", "No titles match the filter"}, {"Lydforsinkelse", "Audio delay"}, {"Ingen", "None"}, {"Strøm", "Stream"}, {"Beholder", "Container"}, {"Buffer", "Buffer"},
        {"%.0f s fremover", "%.0f s ahead"}, {"Køer", "Queues"}, {"video %d  \xC2\xB7  lyd %d pakker", "video %d  \xC2\xB7  audio %d packets"},
        {"Rebuffringer", "Rebuffers"}, {"Video", "Video"}, {"Fargeområde", "Colour"}, {"Dekoder", "Decoder"},
        {"Maskinvare (sceVideodec2)", "Hardware (sceVideodec2)"}, {"Programvare (FFmpeg)", "Software (FFmpeg)"},
        {"Skjerm", "Display"}, {"PCM, %d kanaler", "PCM, %d channels"}, {"Utgang", "Output"},
        {"Metode", "Method"}, {"Hvorfor", "Why"}, {"Se rulletekst", "Watch credits"}, {"Lukk", "Close"}, {"Slett", "Delete"},
        {"Bytt bruker eller server", "Change user or server"}, {"Spilles om %d s", "Plays in %d s"}, {"Bildefrekvens", "Refresh rate"},
        {"60 Hz (TV-en har ikke 120 Hz)", "60 Hz (the TV has no 120 Hz)"}, {"Se sammen", "Watch Together"}, {"Forlat gruppe", "Leave group"}, {"Lag ny gruppe", "Create group"},
        {"Ingen andre grupper akkurat nå.", "No other groups right now."}, {"○ tilbake", "○ back"},
        {"Jelly5: startes for hele gruppen", "Jelly5: starting for the whole group"},
        {"Du er med i en gruppe. Det noen i gruppen starter, spilles for alle, i takt.",
         "You're in a group. Whatever anyone in it starts plays for everyone, in step."},
        {"Se det samme samtidig som andre på denne Jellyfin-serveren, i takt. Den som starter noe, starter det for alle.",
         "Watch the same thing as others on this Jellyfin server, in step. Whoever starts something starts it for everyone."},
        {"Konto", "Account"}, {"Generelt", "General"}, {"BRUKERNAVN", "USERNAME"}, {"PASSORD", "PASSWORD"}, {"Avspilling", "Playback"}, {"Logg ut", "Sign Out"},
        {"Maks kvalitet", "Maximum quality"}, {"Foretrukket lydspråk", "Preferred audio language"},
        {"Undertekstspråk", "Subtitle language"}, {"Undertekststørrelse", "Subtitle size"},
        {"Undertekstbakgrunn", "Subtitle background"}, {"Spill neste episode automatisk", "Play next episode automatically"},
        {"Hopp over intro automatisk", "Skip intros automatically"}, {"Om Jelly5", "About Jelly5"},
        {"Automatisk (maks)", "Automatic (maximum)"}, {"Versjon ", "Version "}, {"Automatisk", "Automatic"},
        {"Lyd, undertekster og autoavspilling lagres på kontoen din på serveren og gjelder i alle appene du bruker med den.", "Audio, subtitles and autoplay are saved on your account on the server and apply in every app you use with it."},
        {"Språk følger PS5-en, eller velg her.", "The language follows the PS5, or choose it here."},
        {"Jelly5 er fri programvare (GPL-3.0) og bygger på EVO Player og Nuvio PS5.",
         "Jelly5 is free software (GPL-3.0) and builds on EVO Player and Nuvio PS5."},
        /* language names */
        {"Norsk", "Norwegian"}, {"Nynorsk", "Norwegian Nynorsk"}, {"Engelsk", "English"}, {"Svensk", "Swedish"},
        {"Dansk", "Danish"}, {"Finsk", "Finnish"}, {"Tysk", "German"}, {"Fransk", "French"}, {"Spansk", "Spanish"},
        {"Italiensk", "Italian"}, {"Japansk", "Japanese"}, {"Koreansk", "Korean"}, {"Kinesisk", "Chinese"},
        {"Portugisisk", "Portuguese"}, {"Russisk", "Russian"}, {"Nederlandsk", "Dutch"}, {"Polsk", "Polish"},
        {"Islandsk", "Icelandic"},
        /* Seerr: settings and sign-in */
        {"Jellyfin-passord", "Jellyfin password"}, {"Seerr-konto (e-post)", "Seerr account (email)"},
        {"Automatisk (Quick Connect)", "Automatic (Quick Connect)"}, {"Adresse", "Address"},
        {"Pålogging", "Sign-in"}, {"Seerr-konto", "Seerr account"}, {"Nettverk", "Network"},
        {"Test tilkoblingen", "Test connection"}, {"Jellyfin-passord for ", "Jellyfin password for "}, {"Emby-passord", "Emby password"}, {"Emby-passord for ", "Emby password for "}, {"Server", "Server"},
        {"E-post for Seerr-kontoen", "Seerr account email"}, {"Passord for Seerr-kontoen", "Seerr account password"},
        {"Ikke angitt", "Not set"}, {"Internett", "Internet"}, {"Bare lokalt nettverk", "Local network only"},
        {"✕ logg ut", "✕ sign out"}, {"Svarer ikke – prøver igjen", "Not answering – trying again"},
        {"✕ godkjenn Quick Connect for denne adressen", "✕ approve Quick Connect for this address"}, {"Automatisk pålogging mislyktes – velg passord", "Automatic sign-in failed – choose a password"},
        {"Ikke pålogget – ✕ for å logge på", "Not signed in – ✕ to sign in"}, {"Tester …", "Testing …"},
        {"✕ for å teste", "✕ to test"}, {"Seerr-adresse", "Seerr address"},
        {"Seerr henter alt fra TMDB selv: PS5-en snakker bare med serveren din og Seerr.", "Seerr gets everything from TMDB itself: the PS5 only talks to your server and Seerr."},
        {"Seerr svarer ikke på ", "Seerr is not answering at "}, {"Ingen Seerr-server på ", "No Seerr server at "},
        {" – bruk den lokale adressen", " – use its local address"},
        {"Seerr %s svarer, men du er ikke pålogget", "Seerr %s answers, but you are not signed in"},
        {"OK – Seerr %s, pålogget som %s", "OK – Seerr %s, signed in as %s"},
        {" – men bildene kommer ikke", " – but its pictures don't come through"},
        /* Seerr: where a title stands, search */
        {"Venter på godkjenning", "Pending approval"}, {"Venter", "Pending"}, {"Forespurt", "Requested"},
        {"Delvis tilgjengelig", "Partially available"}, {"Tilgjengelig", "Available"}, {"Blokkert", "Blocklisted"},
        {"Ikke forespurt", "Not requested"}, {"Fra Seerr", "From Seerr"}, {"Seerr svarer ikke", "Seerr is not answering"},
        {"Ikke pålogget Seerr – se Innstillinger", "Not signed in to Seerr – see Settings"},
        {"Ingenting mer på Seerr", "Nothing more on Seerr"},
        /* Seerr: a title's page and requests */
        {"Allerede forespurt", "Already requested"}, {"Kvoten for forespørsler er nådd", "Request quota reached"},
        {"Du har ikke lov til å be om dette", "You are not allowed to request this"},
        {"Seerr-økten var utløpt – logger på igjen, prøv på nytt", "The Seerr session had expired – signing in again, try again"},
        {"Forespørselen mislyktes", "The request failed"}, {"Velg minst én sesong", "Choose at least one season"},
        {"Godkjennes automatisk og sendes rett videre", "Approved automatically"},
        {"En administrator må godkjenne den", "An administrator must approve it"},
        {"%d av %d forespørsler brukt (siste %d dager)", "%d of %d requests used (last %d days)"},
        {"Henter valg …", "Loading options …"}, {"Sender …", "Sending …"}, {"Be om «%s»", "Request “%s”"},
        {"Serie", "Series"}, {"Film", "Movie"}, {"Alle sesonger", "All seasons"},
        {"Alle manglende sesonger", "All missing seasons"}, {"%d sesong", "%d season"}, {"%d sesonger", "%d seasons"},
        {"%d episoder", "%d episodes"}, {"Kvalitetsprofil", "Quality profile"}, {"Rotmappe", "Root folder"},
        {"  ·  %lld GB ledig", "  ·  %lld GB free"}, {"Be om", "Request"}, {"Prøv igjen", "Try again"},
        {"Trailer", "Trailer"}, {"Skann med telefonen for å se den der", "Scan with your phone to watch it there"},
        {"Forespørselen er godkjent – den hentes snart", "Approved – it will be downloaded soon"},
        {"Forespørselen er sendt – venter på godkjenning", "Request sent – waiting for approval"},
        {"Ingenting å be om: alt er der eller forespurt allerede", "Nothing to request: everything is there or requested already"},
        {"Se i biblioteket", "View in library"},
        {"Kunne ikke hente detaljene fra Seerr", "Couldn't get the details from Seerr"},
        {"Seerr-kontoen din kan ikke be om serier", "Your Seerr account can't request series"},
        {"Seerr-kontoen din kan ikke be om filmer", "Your Seerr account can't request movies"},
        {"Be om flere sesonger", "Request more seasons"},
        /* Seerr: the Discover tab */
        {"Oppdag", "Discover"}, {"Trender nå", "Trending"}, {"Populære filmer", "Popular movies"},
        {"Populære serier", "Popular TV shows"}, {"Kommende filmer", "Upcoming movies"},
        {"Kommende serier", "Upcoming TV shows"}, {"Mine forespørsler", "My requests"},
    };
    return t;
}

} // namespace

const char *choice_name(int choice)
{
    static const char *const names[ChoiceCount] = {"",        "Norsk",   "English",   "Espa\xC3\xB1ol",
                                                   "Fran\xC3\xA7" "ais", "Deutsch", "Portugu\xC3\xAAs", "Italiano"};
    return choice > 0 && choice < ChoiceCount ? names[choice] : "";
}

void set_choice(int choice)
{
    Lang l;
    if (choice > Auto && choice < ChoiceCount) {
        l = (Lang)(choice - 1);
    } else {
        int sys = kSystemNorwegian;
        if (sceSystemServiceParamGetInt(kParamLang, &sys) != 0)
            sys = kSystemNorwegian;
        l = from_system(sys);
        evo_bt("i18n: system language %d", sys);
    }
    if ((int)l != s_lang.exchange((int)l))
        s_gen++;
}

Lang lang() { return (Lang)s_lang.load(); }
unsigned generation() { return s_gen.load(); }

} // namespace i18n

const char *T(const char *nb)
{
    const i18n::Lang l = i18n::lang();
    if (!nb || l == i18n::Lang::Norwegian)
        return nb;
    const std::unordered_map<std::string, const char *> *own =
        l == i18n::Lang::Spanish      ? &i18n::spanish_table()
        : l == i18n::Lang::French     ? &i18n::french_table()
        : l == i18n::Lang::German     ? &i18n::german_table()
        : l == i18n::Lang::Portuguese ? &i18n::portuguese_table()
        : l == i18n::Lang::Italian    ? &i18n::italian_table()
                                      : nullptr;
    if (own) {
        const auto it = own->find(nb);
        if (it != own->end())
            return it->second;
    }
    const auto &t = i18n::english_table();
    const auto it = t.find(nb);
    if (it != t.end())
        return it->second;
    static std::mutex lock;
    static std::set<std::string> missing;
    std::lock_guard<std::mutex> g(lock);
    if (missing.insert(nb).second)
        evo_bt("i18n: no English for \"%s\"", nb);
    return nb;
}
