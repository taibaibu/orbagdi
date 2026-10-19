#pragma once

// ---------------------------------------------------------------------------
// A1: Flash download - the FlashOS path.
//
// "FlashOS" is Keil's name for the whole flash-programming system: a FlashDevice
// description plus a set of programming functions (Init / EraseSector /
// ProgramPage / Verify / ...) that run on the target out of a .FLM image, with
// the driver as the host side of it.  Both halves are here.
//
// The work is split the non-obvious way round: uVision hands the programming
// job to the driver DLL and expects the DLL to pull the image from it.
//
//   AG_Init(AG_INITITEM | AG_INITFLASHLOAD)  -> load the .FLM into target RAM
//                                               and run its Init()
//   AG_Init(AG_INITITEM | AG_STARTFLASHLOAD) -> EraseAll / WriteAll / VerifyAll
//   pCbFunc(AG_CB_GETFLASHPARAM)             -> DLL pulls {start,many,image}
//   DllUv3Cap(100)                           -> advertises the capability
//   AG_MemAcc(AG_F_ERASE)                    -> Flash / Erase in uVision's menu
//   AG_MemAcc(AG_F_RUN)                      -> Flash / Run
//
// A device whose CMSIS-Pack debug description carries flash sequences is flashed
// through uVision's sequence interpreter instead of an FLM; FlashInitLoad()
// picks between the two and the rest of the module follows that choice.
// ---------------------------------------------------------------------------

namespace orb {
namespace agdi {

// Sniff the FLM path and RAM window out of the driver option string uVision
// keeps in the project (EnumUvARM7 gets it as DEV_X66::TargArgs).
// Accepted forms:  -FL"<file>.FLM"   flm=<file>   ram=<addr>,<size>
void FlashOptions(const char* targArgs);

const char* FlashFilePath();       // "" when no algorithm is configured
bool        FlashAlgorithmReady();

bool FlashInitLoad();              // AG_INITITEM | AG_INITFLASHLOAD
bool FlashStartLoad();             // AG_INITITEM | AG_STARTFLASHLOAD
void FlashUnInit();                // AG_UNINIT / target unhook

// AG_MemAcc(AG_F_ERASE): uVision's Flash / Erase.  Brings the algorithm up when
// no download has run yet, then erases the whole device.
bool FlashEraseChip();

// AG_MemAcc(AG_F_RUN): uVision's Flash / Run - reset the target and let the
// freshly programmed application go.
bool FlashRunApp();

const char* FlashLastError();

} // namespace agdi
} // namespace orb
