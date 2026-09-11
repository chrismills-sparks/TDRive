// TDRiveTOP.h
//
// Cross-platform TouchDesigner Custom TOP that renders a .riv via Rive's
// PLS renderer. All platform-specific GPU code lives in an IBackend
// implementation; this class handles file/scene/view-model state,
// parameters, CHOP/DAT inputs, and the Info DAT.

#pragma once

#include "TOP_CPlusPlusBase.hpp"

#include <chrono>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "rive/file.hpp"
#include "rive/artboard.hpp"
#include "rive/scene.hpp"
#include "rive/animation/state_machine_instance.hpp"
#include "rive/viewmodel/runtime/viewmodel_instance_runtime.hpp"

#include "IBackend.h"

class TDRiveTOP : public TD::TOP_CPlusPlusBase {
public:
    TDRiveTOP(const TD::OP_NodeInfo* info, TD::TOP_Context* context);
    ~TDRiveTOP() override;

    // TOP_CPlusPlusBase overrides
    void getGeneralInfo(TD::TOP_GeneralInfo*, const TD::OP_Inputs*, void*) override;
    void execute(TD::TOP_Output*, const TD::OP_Inputs*, void*) override;
    void setupParameters(TD::OP_ParameterManager*, void*) override;
    void pulsePressed(const char* name, void*) override;
    void getErrorString(TD::OP_String* error, void*) override;
    void buildDynamicMenu(const TD::OP_Inputs*, TD::OP_BuildDynamicMenuInfo*, void*) override;
    bool getInfoDATSize(TD::OP_InfoDATSize*, void*) override;
    void getInfoDATEntries(int32_t index, int32_t nEntries,
                           TD::OP_InfoDATEntries* entries, void*) override;

private:
    bool loadFileIfNeeded(const char* absPath);
    // Parses 'absPath' into a menu-only rive::File (cached) and returns it, or
    // nullptr if it can't be read. See mMenuFile for why this is separate from
    // the file execute() renders.
    const rive::File* menuFileFor(const char* absPath);
    bool selectArtboardIfNeeded(const char* name);
    bool selectSceneIfNeeded(const char* stateMachineName);
    void applyInputsFromCHOP(const TD::OP_CHOPInput* chop);
    void applyStringsFromDAT(const TD::OP_DATInput* dat);
    void bindArtboardViewModel();
    // Texture injection (Image1..N params -> view-model image properties).
    // CPU download path; used on macOS and as the Windows non-CUDA fallback.
    void applyImageInputsCPU(const TD::OP_Inputs* inputs);
    // Binds 'img' to the named view-model image property (once per change).
    void bindSlotImage(int slot, const char* propName, rive::RenderImage* img);
    rive::StateMachineInstance* currentSMI() { return mSMI; }
    void setError(const std::string& s) { mError = s; }
    void clearError()                   { mError.clear(); }

    TD::TOP_Context*               mContext = nullptr;
    std::unique_ptr<tdrive::IBackend> mBackend;
    bool                           mBackendReady = false;

    // Loaded file + scene
    rive::rcp<rive::File>                       mFile;
    std::unique_ptr<rive::ArtboardInstance>     mArtboard;
    std::unique_ptr<rive::Scene>                mScene;
    rive::StateMachineInstance*                 mSMI = nullptr;  // non-owning
    rive::rcp<rive::ViewModelInstanceRuntime>   mVMRuntime;

    std::string mLoadedPath;
    std::string mLoadedArtboard;
    std::string mLoadedStateMachine;

    // Menu-only copy of the .riv, parsed with a CPU-only factory.
    //
    // buildDynamicMenu() is called on TouchDesigner's main thread whenever the
    // Artboard / State Machine menus are opened, which is a different thread
    // from the one execute() cooks on. It must not reach into mFile/mArtboard/
    // mScene (the cook thread owns those, and re-importing through them used
    // to tear down the animation that was already playing), and it must not
    // depend on the GPU backend having come up - the menus have to work even
    // when the render context failed to initialize. So the menu keeps its own
    // parse, cached by path.
    std::unique_ptr<rive::Factory> mMenuFactory;
    rive::rcp<rive::File>          mMenuFile;
    std::string                    mMenuPath;

    // Last File / Artboard parameter values execute() read, captured *before*
    // it opens the CUDA bracket. In CUDA execute mode TouchDesigner refuses
    // OP_Inputs / OP_Parameters access once beginCUDAOperations() has run for
    // the node ("OP_Inputs and OP_Parameters can not be used after
    // beginCUDAOperations() has been called"), and buildDynamicMenu() is
    // handed an OP_Inputs it therefore cannot read - so the menus read these
    // instead. See buildDynamicMenu().
    std::string mLastParFilePath;
    std::string mLastParArtboard;

    // Set once execute() has opened the CUDA bracket for this node, which is
    // the moment the OP_Inputs handed to buildDynamicMenu() stops being
    // readable. Before that - a node that has not cooked yet - reading the
    // parameters directly still works and is fresher than the cache.
    bool mCudaBracketUsed = false;

    // Playback timing
    std::chrono::steady_clock::time_point mLastTick;
    bool mHasTick = false;

    // Edge detection
    std::unordered_map<std::string, float>       mPrevChopValues;
    std::unordered_map<std::string, std::string> mPrevDatValues;

    // Texture injection state. mPendingDl holds the async CPU download
    // started on the previous cook (1-frame latency avoids a GPU stall);
    // mBoundSlotImage tracks which RenderImage each VM property last saw so
    // we only rebind on identity change.
    TD::OP_SmartRef<TD::OP_TOPDownloadResult> mPendingDl[tdrive::kMaxImageSlots];
    rive::RenderImage*   mBoundSlotImage[tdrive::kMaxImageSlots] = {};
    std::vector<uint8_t> mPremulScratch;

    std::string mError;
};
