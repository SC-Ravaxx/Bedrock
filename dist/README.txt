BEDROCK - Mining Companion for Star Citizen (4.10)
=================================================

What it does
------------
Reads the signature value off the mining marker in game (the number next to
the map pin, e.g. "16,960") straight from the screen and tells you which ore
in what amount is behind it. On top of that: a place map per ore (click a
result row) and the ORE FINDER (key F), where you pick an ore BEFORE the trip
and see where the odds stand - and where it sells best.

Getting started
---------------
1. Unpack the folder anywhere (not into "Program Files" - the app writes into
   its own folder).
2. Start Bedrock.exe (best while Star Citizen is already running). Windows
   SmartScreen speaks up on the first start because the exe is not signed:
   "More info" and "Run anyway".
3. Search area: the app works it out itself - from the game window, on the
   monitor the game runs on, in proportion to the resolution, around the
   crosshair. If the game changes monitor or resolution, the area follows.
   There is nothing to set, and nothing is drawn on the screen for it
   (overlay=1 in region_config.txt draws the frame, for a look).
   (Until 3.8 the box could also be typed in by hand, with an APPLY and an
   AUTO button. The automatic box has always been the right one and the two
   buttons only caused confusion, so they are gone. If some case really needs
   a fixed box, region_config.txt takes fixedaim=1 together with x, y, width
   and height - written by hand, on purpose.)
4. LIVE is already on at start: the app scans about three times a second, and
   the search frame follows the marker by itself. The LIVE button turns the
   scanning off and on again.
   The header says where you are ("You are at: Pyro - Monox") and which ship
   you sit in, both read off the game's own log: the route the ship computes
   names the place it starts from, the arrival after a quantum jump names the
   target, a gateway right after a jump point names the far side. The ore
   candidates are then narrowed to the ores the tables list for that place -
   at Monox only Monox's ores stay in the running, the rest are marked "not
   at". On a planet or moon a 2,000 reads "Harvestable/Salvage", in space
   "Salvage" - there is no salvage on a body and no harvestable in space.
   (The three system buttons are gone with it: the log says which system you
   are in, and better than a button you forget to press.)
5. I'M LOOKING FOR, under the header, is the list of ores you want to be told
   about. Open it, type a few letters to narrow the list, click as many ores
   as you like. A match then blinks red and carries the word LOOKING FOR, so
   you can watch the game rather than this window. Whatever a recipe on your
   shopping list needs blinks the same way on its own, marked ON YOUR LIST.
   The pick is kept in looking_for.txt and survives a restart.
6. OVERLAY puts the compact view over the game - it opens where you last
   put it; ORE FINDER (F) opens the ore search, BLUEPRINTS (B) the crafting
   recipes, REFINERY (R) the station table, NOTES (N) your own notebook.
7. SHIP, next to PLACE, holds the ships you mine with and what they carry -
   heads, modules, crafted bonus - which is what the rock view works with.
   You can save several, also more than one of the same type, each under its
   own name: add one at the bottom of the list, change it with "Edit ship".
   Pick a ship once while you're sitting in it, after that Bedrock recognizes
   it by itself. (The game's log names each ship by an id. When that id
   changes, pick the ship once more.)

Refinery (key R)
----------------
Twenty refineries, and how much of a load each one gives back. Tick the
materials in your hold on the left and the stations sort themselves by what
comes back out; tick nothing and the list shows instead what each station is
GOOD at. The tabs narrow it to one system.

Work orders: when you use a refinery terminal (REFINEMENT CENTER), Bedrock
reads your work orders off it and lists them under Work orders, the tab on
the right: what comes out of each and when it's done. The Refinery button in
the main window shows how many are done. Orders you collect at the terminal
come off the list by themselves, and "Collected" takes one off by hand.
Bedrock only looks for the terminal while you're on foot, not while you're
mining. Tested at Levski, Checkmate and Pyro Gateway.

A dash means the station's profile names no modifier for that material -
which is not the same as none, and the app will not print a nought it
cannot stand behind. For that reason stations that list EVERYTHING you
ticked are ranked first, and the rest follow marked "2 of 3 listed".

The method is picked from the dropdown under the tabs, and every figure
on the page follows it. The game rates its nine methods only in words -
Dinyx Solventation is "Low Speed // High Cost // High Yield" - and the
numbers behind the words are in no game file. The factor tables the app
uses (yield 1.00 / 0.85 / 0.70, cost 1 / 2 / 6, and the times) come from
the community's refining library and were checked against nine real
work orders at Levski, one per method, on the same load of
aluminium: all 27 yields came back to the unit, the station's own
modifier included. The bill and the clock are measured for aluminium
only; the other ores are scaled to match it.

Blueprints (key B)
------------------
Every one of the 1,606 crafting recipes in the current build. Just start
typing and it searches item, component and material in one go ("helmet",
"shield", "aslarite"). On the left the list can be switched: BY MATERIAL
shows the 37 materials with the number of their recipes, BY COMPONENT shows
the parts by SHIP (shield, power plant, cooler, quantum, radar, weapons ...),
ARMOR & CLOTHING (helmet, core, arms, legs ...) and HANDHELD. Both can be
combined, Esc clears search and filters again, the wheel scrolls. The window
opens on the ore the scan is showing at that moment.

Above the list are three more filters, offering only the values the current
selection really contains: GRADE (A to D), SIZE (the component size) and
CLASS (Civilian, Competition, Industrial, Military, Stealth). Armour and
handhelds carry none of them, and there the bar stays empty.

Clicking a recipe opens it: every slot with its name (Frame, Emitter,
Aperture Iris ...), the material and how much of it, the two best places to
mine it, and a quality slider from 1 to 1000. The slider shows at once what
quality does to the item - "Integrity x1.042  +4.20 %" - and the total over
all slots stands at the bottom. MIN / BASE / 50 % / MAX set every slider at
once.

SNAP (the default) makes the slider land on the eight quality steps the
material really comes in - Agricium rolls 346, 588, 667, 796, 852, 943, 971
or 1000, Titanium other values. The step is named "B4" beside the number.
Above the slider those eight values stand as a comb; the tooth the knob
stands on lights up. The knob is a ring so that it cannot cover the comb.

Three ways to move the slider, because dragging alone was too fiddly:
  - drag it as before,
  - click a tooth of the comb to jump exactly onto that step,
  - arrow keys: left/right one step, up/down picks the slot, Home/End to
    minimum/maximum. In FREE the arrows move in steps of 5, with Shift 25.
Quality 500 means the item comes out exactly like the shop version. Because
500 sits on no step for most materials, that mark now stands below the slider
and is clickable itself - so the shop state is one click away even with SNAP
on. 1000 gives the full bonus, 1 the full penalty.

Clicking a material inside a recipe opens the Ore Finder on that very
material - straight from "what do I need" to "where do I get it".

Under the slots stands WHO hands the blueprint over: the contracts that carry
it as a reward - all of them, sorted by employer, with nothing pre-picked.
With employer, jurisdiction and the reputation rank needed; the wheel scrolls
the list.

Where a blueprint is available in several systems (91 are, some with 23 to 28
contracts), a choice of all / Stanton / Pyro / Nyx appears above the list.

Behind every row stands "1 of 6": that many blueprints share this reward
pool. A real drop chance exists on only seven contracts in the whole game
(the Rayari research runs at 25 % and two bounties at 75 %) - there it is
shown in colour. Every other one hands the blueprint over for certain, as
long as you do not own it yet.

Where a contract is handed in matters: the same employer has its own reward
pool per system, and Pyro is split into four regions on top of that. That is
why every row says "Stanton", "Pyro A/B" or "Nyx" - the FR-86, for instance,
only drops in Stanton even though Foxwell hands out the same contracts in
Pyro and Nyx as well.

Clicking a contract row lays a card over the recipe with everything the game
files hold on that run.

DIFFICULTY is one figure from 1 to 7, not four. The game rates a contract on
four axes (skill, concentration, risk, knowledge) but it also names the
weights that turn them into a single number - 0.34 / 0.33 / 0.22 / 0.11 for
the general profile, with four other profiles for hauling and events - so
that one number is the game's own arithmetic, not an average someone made up.
Under it stands what the rating means in the game's own words: "player might
die, ship could explode", "ship could get damaged, could lose cargo".

Then: the time limit; ENEMY SHIPS, how many come at you at once (1 to 8;
which ships they are sits behind spawn tags the game database does not spell
out, so the count is all that can honestly be said); PARTY, how many players
may take it; the cooldown after finishing and after abandoning (some
contracts are gone for good once abandoned - that stands there in red); the
blueprint chance; the reputation; the other blueprints from the same reward
pool; and the full briefing text. Click anywhere or press Esc to close.

REPUTATION comes from the briefing, where the game writes "Reputation
Awarded: 150" - 2,748 contracts say it there against 461 that put it in their
title, and no contract does both, so together they cover 3,194 of the 3,717
rows. That generated tail of the briefing (the reputation line and the list
of pool blueprints) is cut off before the text is shown, because the card
already carries both as rows of its own.

What is NOT on the card is the payout in aUEC - and not out of neglect:
2,356 contracts point at a "calculated reward", the number is worked out at
hand-in from difficulty and distance and stands in no file. Nothing else in
the game database holds one either - the only money figures anywhere are the
multipliers for a partly finished hauling run, which is a different kind of
contract. The card says so rather than inventing something.

The colour says whether it gets better: green is an improvement, red is not.
On recoil and quantum fuel burn a FALLING value is the improvement, which is
why a green minus figure turns up there now and then.

On 390 of the 4,286 slots quality does not move the value at all. The recipe
still prescribes the material - only the quality is free. There it says "any
quality will do here" and the slider is drawn muted: the cheapest ore of that
kind will do.

On another 237 slots it says "the game files will not say what quality does
here". That is a different thing: a curve DOES exist there, but the tool that
unpacks the game database chokes on exactly those arrays (476 of 6,402) and
writes its error message in place of the numbers. 234 of them could be
recovered, because the same slot reads cleanly on other blueprints and
carries the same value everywhere - that is how the Palatino armour got its
Casing Weave back (x0.925 to x1.075 Damage Mitigation), which had wrongly
stood there as "makes no difference". For the last 237 the app would rather
say it does not know than invent a number.

Your library: which blueprints you already have
-----------------------------------------------
The app knows which blueprints you own - without you clicking anything. Every
time the game flashes up "Received Blueprint: ...", it writes that same line
into its log file (Game.log in the game folder), and the launcher keeps every
old log file under logbackups\. On start the app reads through those backups
once (about 1,000 files in half a minute, in the background, the window stays
usable) and after that the running log file every two seconds. A blueprint
you just collected is in the list before you are back at the bench.

How the app finds the log file: from the running game (the game process gives
away its folder), else from the RSI launcher (which remembers where it last
started the game from), else from region_config.txt if it holds
gamelog=<path to Game.log>. If nothing is found, the window says so, and
ticking by hand still works.

In the list every recipe has a tick box: filled = owned. The LIBRARY filter
(all / N owned / missing) shows only yours or only the ones you are missing -
"missing" together with a material is the shopping list. Top right inside a
recipe it says IN YOUR LIBRARY (with the date the game handed it over) or
MARK AS OWNED; a click flips it - for everything the log never saw (bought,
traded, older than the backups). What you tick by hand overrules the log.

All of it lives in owned_blueprints.txt in the data folder, one line per
blueprint with date and source (log, hand, or assumed from a contract). Deleting the file means it is
rebuilt from the logs on the next start. Blueprints the log knows but this
build no longer has (renamed or removed) stay in the file as "no row in this
build" and are listed in debug\session.txt.

What the log does NOT know: blueprints from the shop or from Wikelo, as long
as the game shows no "Received Blueprint" notification for them - that is
what the button is for.

Some of those gaps close by themselves. A contract that pays out blueprints
gives you one every time you complete it, as long as one of its set is still
missing - and nothing once you have them all. So when you complete such a
contract and no blueprint comes, the app takes the rest of its set as owned
(the recipe then says "assumed - its contract paid none on ..."). It only
does this for contracts that always pay out and whose set does not depend
on the region, and only from game logs of 4.10 on. Should one of them turn
up later after all, the app drops what it assumed from that contract and
assumes nothing from it again.

The list: the + at the right of a recipe puts it on a list, and "WHAT TO
MINE FOR ALL OF IT" under the list adds up the ore every recipe on it needs
- as refined material, and as what has to come out of a rock once the
refinery has kept its share. Two dropdowns over that table say where it is
refined and by which method: the twenty stations with their system, the
nine methods with their factors. The station that would leave the least to
mine is suggested beside them; a click takes the suggestion.

Channels: LIVE and HOTFIX are the same server and the same character.
Whether HOTFIX is a junction onto LIVE on your machine (usually) or a folder
of its own - the app reads both folders as ONE library and remembers per
folder what it has read; a junction is not read twice. PTU, EPTU and
TECH-PREVIEW are copies of your character: if the game runs from there, the
app keeps a file of its own for it (owned_blueprints_PTU.txt) so that nothing
gets mixed up. Which channel is in force stands in debug\session.txt behind
"game log:".

Your blueprints in the game's contracts: tick "Show them in the game's
contracts" at the top of the Blueprints window, and the game's own mission
texts carry what the app knows. Contracts that pay out blueprints get [BP]
in their title - [BP*] where the same title is also used by a variant that
pays out nothing - and their description lists the blueprints, one per line:
the ones you are still missing in blue, the ones already in your library
in plain white. The [BP] is blue while anything is missing. The game
reads its texts when it starts, so it shows your library as it stood then;
the app writes the file again whenever a blueprint comes in, and the next
start shows it.
The file is data\Localization\english\global.ini beside the game's Data.p4k:
the game's own English text, taken out of Data.p4k, with only those marks
added. If user.cfg sets no language, the app adds g_language = english -
without it the game does not look at the file. A language file that is not
the app's (a translation, a community pack) is left alone, and so is a game
set to another language; the window says so. Untick the box and the app
deletes its file again.

Notes (key N)
-------------
A notebook of your own, with folders: routes, prices, who owes you what, the
rock that was worth the detour. Folders on the left, the note on the right in
a real edit box - so selection, Ctrl+C, Ctrl+V, Ctrl+Z, Ctrl+A and the wheel
all work the way they do everywhere else. The text is fixed pitch, because
half of what goes in here is coordinates and quantities that only line up in
a fixed pitch.

NEW FOLDER and NEW NOTE ask for a name (Enter makes it, Esc drops it). With a
note open, RENAME and DELETE sit top right; with only a folder picked, the
same two act on the folder. Clicking a folder that is already picked folds it
shut. Notes save themselves a moment after you stop typing, and again when
you close the window - there is no save button and nothing to forget.

Where it all lives: notes\<folder>\<note>.txt in the data folder (see
"All of it sits in Bedrock's data folder" below). A note IS a
text file - open it in Notepad, copy it, put it in your backup, write one
from outside and the app picks it up with its first line as the preview.
Nothing is packed into a database.

Deleting asks first: the first click on DELETE turns it red and makes it say
SURE? DELETE; a second click within a few seconds does it, any other click -
or just waiting - takes the question back. No dialog box to click away.

What is deleted is not gone. It moves to notes\.trash\, under the folder it
came from and with the date on it, and the row TRASH at the foot of the tree
lists all of it: name, when, where from. RESTORE beside an entry puts a note
back into its folder (made again if the folder itself is gone) and brings a
folder back whole, notes and all. A mis-click on an evening's work is one
click to undo, not a loss. To make room, empty the .trash folder by hand -
the app never erases anything on its own.

Links, the way autocorrect works
--------------------------------
Write "heading to daym" and a small list drops under the caret: Daymar, a
place. Enter links it, a click links it, and carrying on typing ignores it -
exactly like the word suggestion on a phone. It knows every ore, every mining
place, every system and every recipe in the build, and it offers them
half-typed, across a space ("Palatino Arm" finds the Palatino Arms), and
whether or not the name is capitalised.

You do not have to start at the beginning of a name. "Daystar" finds the four
Palatino Daystar recipes, although not one of them is called that - any word
in a name will do, which is the point: you type the part you remember. And
where a word is shared by many, all of them are there. "Palatino" is 23
recipes; eight are shown at a time, and the arrow keys, Page Up/Down or the
wheel go through the rest. The foot of the list says which one of how many
you are on.

It suggests and you decide, rather than deciding for you. That is on purpose:
there is an item in the game called STOP, and a note that says "Wala is the
better stop" should not light that up.

A confirmed link is written into the note as [[Daymar]]. Nobody types those
brackets - the suggestion puts them there - but they are what keeps the link
when the file is opened in Notepad, mailed to someone, or edited outside.

Ctrl+click a link to open it. An ore opens the Ore Finder on it, a recipe
opens the Blueprints window on it, a place opens the Ore Finder the other way
round - WHAT IS MINED AT DAYMAR, every ore there with its chance - and a
system opens the Ore Finder narrowed to it, so [[Nyx]] is everything mined in
Nyx and nothing else.

Resolution: search box and search area are worked out from the height of the
game window (1080p, 1440p, 4K); the reader itself does not depend on the size
of the digits. The Windows scaling (100 %, 125 %, 150 %) makes no difference.

Keys: L = live on/off, F = Ore Finder, B = Blueprints, N = Notes,
O = Overlay, Q / Esc = quit, +/- = scan interval.
Ctrl+Alt+S (works in game too) = snapshot for a bug report, see below.

What the app writes to disk
---------------------------
Your settings (region_config.txt), your blueprint library
(owned_blueprints.txt, with the log lines it came from), your own notes under
notes\, your refinery work orders (refinery_orders.txt), a handful of small
files that remember your choices (the place set by hand, the ores you look
for, the loadout, the shopping list, the window) and, in the folder debug\:
  - scan_log.csv   a text log with one line per scan, rotates at 10 MB
  - session.txt    what the app found at this start (monitors, game window,
                   search area, capture path, game log) - one file, it gets
                   overwritten
  - snapshot_*.png/.txt   only when you press Ctrl+Alt+S; fifty at most, after
                   that the newest replaces the oldest
  - region_config.txt     a copy of your settings, so that the folder debug\
                   holds everything on its own
  - uex_last_screenshot.jpg   only if you report prices to UEX: the last
                   screenshot sent with a report (see What Bedrock sends);
                   the next one replaces it
No screen recording, no video.
With the box in Blueprints ticked (see Your library), also game_text_LIVE.ini
- the game's text, kept until the game's files change - and, in the game's
folder, data\Localization\english\global.ini and one line in user.cfg.

All of it sits in Bedrock's data folder, %LOCALAPPDATA%\Bedrock - not beside
Bedrock.exe, so that replacing or deleting the program folder on an update
can never take your things with it. Backing up is copying that folder: your
settings, your notes and your blueprint library come along. Or click
"Backup / restore data" at the foot of the window: Back up writes all of it
into one .bedrock file, Restore reads it back in (a .txt backup from an
older version too). Leave that file as it is - edited in a text editor, it
will not restore. A file called portable.txt beside Bedrock.exe keeps
everything in the program folder instead, for a USB stick. Nothing is
written to the registry.

What Bedrock sends
------------------
Two network requests of its own, and a third only if you switch it on:

- The update check: one small text file from the-forsaken.org, once at start
  and once an hour. It carries the version and whether it is the Windows or
  the Linux build - nothing about you, your machine or your game. update=0 in
  region_config.txt turns it off, and an update itself is only downloaded
  when you click it.
- Ore prices from UEX: while the Ore Finder is open, Bedrock downloads the
  price list from uexcorp.space (at most every 30 minutes) and UEX's list of
  terminals (once a week). The UEX market tab loads the player listings for
  the selected ore, and the rock view checks them when a rock contains ore of
  quality 900 or higher (each ore at most every 10 minutes). These requests
  include Bedrock's version, the platform and Bedrock's UEX app token, which
  identifies the app, not you. The lists are cached in the uex folder. Set
  uex=0 in region_config.txt to turn this off.
- Your UEX account and price reporting (both optional): on first start,
  Bedrock asks whether you want to connect your UEX account. You can skip
  this and connect later in the Ore Finder (Connect UEX, bottom right). Get
  key opens your UEX account page, where you'll find your secret key. After
  you paste it, Bedrock asks whether to report prices. If you say no, your
  account stays connected and nothing is reported. If you say yes, Bedrock
  sends UEX every trade you make at a commodity terminal (terminal,
  commodity, price per SCU and time) and every refinery job you start, which
  is added to the refinery jobs on your UEX account. A trade is only
  reported if Bedrock can tell the terminal for certain. UEX only takes
  reports from data runners. If you aren't one yet, click Sign up on UEX
  when Bedrock asks, and register there. UEX reviews new
  data runners during their first 90 days, so for 90 days each price report
  includes a screenshot of the whole game window, taken right after you
  trade (it shows whatever is on screen, such as your name and balance on
  the terminal and the chat if it's open). Bedrock keeps the last
  screenshot it sent in the debug folder (uex_last_screenshot.jpg), and the
  UEX account window has a link that opens it. Bedrock
  asks you about this before anything is sent; if you decline, nothing is
  reported and your account stays connected. Your key is stored only on this
  PC (encrypted for your Windows user) and is never included in backups or
  the debug folder. You can stop reporting at any time in the Ore Finder.
  With your account connected, the UEX market tab also checks your
  conversations and notifications on the UEX marketplace (every two minutes
  while the tab is open), so clicking a listing you've already messaged
  about opens that conversation.

A cleanly read value no table knows (e.g. 70,000) is shown as read after four
identical scans, with the nearest table values beside it - as an unknown
signature, not as a "repaired" neighbouring result.

Rocks with two ores (e.g. Silicon x4 + Riccite) show the SUM of both
signatures; the app then lists the matching pairs as rows of their own
("Silicon x4 + Riccite") - what the rock really holds you see in the scan.

When nothing is read
--------------------
1. Run Star Citizen in borderless window mode ("Borderless"), not in
   exclusive fullscreen - screen capture cannot see that.
2. In game, aim at a rock so the marker with the number is visible, and press
   Ctrl+Alt+S (a short sound confirms it). That writes
   debug\snapshot_<time>.png and .txt: the game picture as the app sees it,
   plus everything it is working with at that moment.
3. Send the whole debug\ folder (snapshot, session.txt, scan_log.csv and the
   copy of the settings are all in there). That is enough to reproduce the
   case at the desk.
4. If you like a console: "Bedrock.exe --capture" reports the capture path,
   HDR status and share of white, and writes debug\capture_check.png.

Notes
-----
- HDR: the app captures via DXGI and copes with HDR; if the share of white in
  "--capture" is above 8 %, the highlights are being clipped - then try HDR
  off.
- Recognition works by shape (pin + row of digits), not by colour: the blue
  marker you get without a helmet, the orange or gold one with a helmet, by
  day and by night, on the Prospector, the MOLE and the Golem alike. The one
  weak spot left is a marker washed out under the mining lamp: without a
  helmet the plate goes nearly white, and there the reader stays silent far
  more often than it guesses - a wrong value is worse than none.

Contact, and the tip jar (the ? in the header)
---------------------------------------------
The ? beside OVERLAY opens a small card with both on it.

Something read wrong, or an idea: Discord .ravaxx - the card copies the
handle to the clipboard when clicked. A snapshot helps more than a
description does: Ctrl+Alt+S while the game runs, then send debug\.

ko-fi.com/ravaxx is there if you want to help with the work and with the
server the updates come from. Bedrock is free and stays free - nothing in
it is held back, and nothing changes if you never look at that link.

Where the tables come from
--------------------------
Signatures, recipes, quality bands and contract sources come from the game
files of Star Citizen 4.10 itself (Data.p4k, unpacked with unp4k/unforge).
Mining places and refinery profiles are compiled from scmdb.net, the refining
tables from Regolith (regolith.rocks). Apart from the update check and UEX
(see What Bedrock sends) the app needs no internet - it reads the screen and
the game's own log file.

The sell prices come from UEX (uexcorp.space), where players report what the
terminals pay. They can be days old, so each one says how old it is, and a
price nobody has reported for 30 days is left out. WHERE TO SELL in the Ore
Finder lists the five terminals that pay most for one SCU of the refined ore,
from all systems; the ore that sells highest right now blinks red in the
list.

Who owns what
-------------
Bedrock is an unofficial fan tool. It is not made by, endorsed by or
affiliated with Cloud Imperium Games. Star Citizen(R), Roberts Space
Industries(R), Squadron 42(R) and Cloud Imperium(R) are trademarks of
Cloud Imperium Rights LLC and Cloud Imperium Rights Ltd; the game data
and the in-game text this app shows are their property, shown here in a
free, non-commercial tool that reads the game the way you do - from the
screen and from the log the game itself writes.

Bedrock is free software under the GNU General Public License, version 3
or later (LICENSE). Its source code is at
https://github.com/SC-Ravaxx/Bedrock. The libraries it is built on are
used under their own licences, reproduced in full in
THIRD-PARTY-NOTICES.txt - that file travels with the package because
those licences require it, not as decoration.
