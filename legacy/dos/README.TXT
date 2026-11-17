TINY 2.1 - a neural network that answers questions, for DOS and Turbo C 2.0
===========================================================================

Files
  TINY.EXE    ready to run, built with Turbo C 2.0 (large model)
  TINY.BIN    the trained network (about 380 KB). Must sit next to TINY.EXE.
  TINY.C      the program source (C, with Turbo C screen and sound parts)
  BUILD.BAT   rebuilds TINY.EXE with the Turbo C command line compiler
  README.TXT  this file

Quick start (no compiling needed)
  1. Copy TINY.EXE and TINY.BIN into one directory, e.g. C:\TINY
  2. Leave Windows (File / Exit Windows) so you are at a plain C:\> prompt.
  3. CD \TINY
     TINY

Memory
  Needs about 540 KB of free conventional memory.
  Type MEM at the DOS prompt and look at "Largest executable program
  size". If it is below 540K, free some memory: put DOS=HIGH,UMB in
  CONFIG.SYS, load drivers with DEVICEHIGH / LOADHIGH (or run MEMMAKER
  on MS-DOS 6), or start without network/CD-ROM drivers (on MS-DOS 6,
  press F8 at boot to choose). It also runs in a Windows 3.1
  "MS-DOS Prompt" if that has enough free memory, but more slowly.

Speed
  It uses floating point. With a math coprocessor (any Pentium, any
  486DX or 486DX2) an answer takes a few seconds. Without one
  (386SX/386DX, 486SX) the coprocessor is emulated in software and an
  answer can take several minutes. MSD.EXE (in C:\DOS or C:\WINDOWS)
  shows "Math Coprocessor: Yes/None" under Computer.

Using it
  TINY starts with a boot sequence (digital rain - any key skips it),
  then a full-screen chat. Type a question and press Enter. A scanner
  sweeps while TINY reads your question, then the answer is decoded
  letter by letter and "spoken" by the robot voice, lighting up each
  word. Press any key to stop the voice.

  The NEURAL MONITOR on the right shows the network working, live:
  LAYER ACTIVITY   how busy each of the 4 layers is for every letter
  CONFIDENCE       how sure TINY is of the letter it just chose
                   (green sure, amber unsure, red guessing)
  LAST LETTERS     the same for the last 18 letters
  SPEED            letters per second, including reading the question
  On VGA screens TINY uses its own neon colours; your normal colours
  come back when it quits.

  Keys     F1 help            F2 sound on/off     F3 robot voice on/off
           F4 creativity 0-9  Up arrow = last question     Esc = quit
  Words    HELP, CLS (clear the screen), QUIT

  Start options (can be combined, e.g. TINY 0 /QUIET):
  TINY 0      always the surest answer (best for quizzes and sums)
  TINY 5      the default: answers are worded differently each time
  TINY 9      the most varied (and makes more mistakes)
  TINY /DEMO  TINY asks itself sample questions - press a key to take over
  TINY /QUIET no sound at all
  TINY /MONO  grey colours, for monochrome or grey LCD screens
  TINY /CLASSIC  standard DOS colours instead of the neon palette
  TINY /P     plain text, no full screen (also used automatically when
              questions come from a file: TINY < QUESTIONS.TXT > ANSWERS.TXT)

Sound and voice
  All sound comes from the PC speaker. The robot voice is not real
  speech: every letter becomes a small sound (vowels buzz, s and f hiss,
  p and t click) with the rhythm of speech - like a film robot. Real
  text-to-speech does not fit next to the network in 640 KB of memory.

Things to try (all answered correctly by this TINY.BIN in testing)
  what is the capital of peru            is a whale a fish
  paris is the capital of which country  what is the capital of texas
  what language do they speak in canada  what money do people use in japan
  which countries border estonia         does france border spain
  what is 7 times 8                      what comes after 199
  which is bigger, 405 or 450            write 47 in words
  is 31 even or odd                      what is 14 in roman numerals
  spell elephant                         what is cat backwards
  what letter comes after k              plural of mouse
  past tense of go                       opposite of hot
  what is another word for big           what is the superlative of good
  what sound does a cow make             what is a baby kangaroo called
  how many legs does a spider have       which animal says hoot
  what is a group of lions called        what is the fastest land animal
  what is the symbol for gold            how many moons does mars have
  how do you say thank you in spanish    who invented the telephone
  what is the biggest ocean              where is the eiffel tower
  how many strings does a guitar have    what is dos
  what time is it                        tell me a joke
  what is 15 minus 6                     how do you say thank you in german
  It understands many wordings, small typos, CAPS LOCK and friendly
  extras like "hey tiny, ..." or "please".

What it knows (about 3,500 facts)
  all 194 countries: capital, continent, region, neighbours, coast,
  people, money and language (for most), internet domain; US state
  capitals; the 118 chemical elements; animals (groups, babies, sounds,
  legs, food, homes, male/female); space, the body, nature, science,
  food, sport, music, history and inventors, computers and DOS;
  opposites, synonyms, plurals, past tenses, good/better/best; simple
  words in French, German, Spanish and Italian; sums, times tables,
  Roman numerals, number words, odd/even, bigger/smaller, spelling.

How good is it
  On 780 test questions it never saw during training (new wordings and
  new combinations) it gets about 61% right. Version 1 got 6% on the
  same test. On questions worded the way it was taught it gets about
  90% right.

Limits
  It is a very small network (367,000 numbers). It can still answer
  confidently and be wrong, especially for:
  - questions about two countries at once, and coast/landlocked
  - rare chemical elements and less known countries
  - division (addition, subtraction and times tables are better)
  - made-up names: it sometimes invents an answer instead of saying
    it does not know
  Sums only go up to 20 + 20 and the 12 times table.

Rebuilding in the Turbo C 2.0 IDE
  1. Copy TINY.C and TINY.BIN into one directory.
  2. Start TC, then File / Load / TINY.C
  3. Options / Compiler / Model / Large                     (important)
  4. Options / Compiler / Code generation / Floating point / Emulation
     (Emulation automatically uses the coprocessor when there is one.
     Choosing 8087 makes an EXE that crashes on machines without one.)
  5. Options / Directories: Include = C:\TC\INCLUDE, Library = C:\TC\LIB
     (or wherever Turbo C is installed)
  6. Compile / Build all.  This makes TINY.EXE.
     Two warnings "Unreachable code in function load_model" are normal.
  7. Quit Turbo C (Alt-X) and run TINY from the DOS prompt.
     Running it inside Turbo C (Ctrl-F9) fails with "not enough memory".

Rebuilding from the command line
  BUILD.BAT, or:   TCC -ml -IC:\TC\INCLUDE -LC:\TC\LIB TINY.C

TINY.BIN format (version 2)
  "TNY2", then D, layers, heads, middle width as 16-bit numbers.
  Each weight table is stored row by row: a 32-bit float scale and then
  one signed byte per weight. Gains are 32-bit floats. TINY.EXE reads the
  sizes from the header, so a retrained TINY.BIN of another size works
  with the same EXE (up to 128 wide, 512 middle, 8 layers).

Tested: built with Turbo C 2.0 (TCC 2.0, TLINK 2.0) and run under DOS.
The TRAINKIT zip has the training data generator and training script.
