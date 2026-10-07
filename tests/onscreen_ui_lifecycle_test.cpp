#include "VideoCommon/OnScreenUI.h"
#include "VideoCommon/AbstractPipeline.h"
#include "VideoCommon/AbstractTexture.h"
#include "VideoCommon/NativeVertexFormat.h"

#include <imgui.h>
#include <imgui_internal.h>
#include <implot.h>

int main()
{
  // Input and cleanup can arrive before Initialize creates the context.
  {
    VideoCommon::OnScreenUI ui;
    ui.SetKey(0, true, "x");
    ui.SetMousePos(12.0f, 24.0f);
    ui.SetMousePress(1);
    if (ImGui::GetCurrentContext())
      return 1;
  }

  // Valid-context events still reach ImGui; partial startup has no open frame.
  {
    VideoCommon::OnScreenUI ui;
    ImGui::CreateContext();
    ImPlot::CreateContext();
    ui.SetKey(0, true, "x");
    ui.SetMousePos(12.0f, 24.0f);
    ui.SetMousePress(1);
    if (ImGui::GetCurrentContext()->InputEventsQueue.Size < 3)
      return 2;
  }
  if (ImGui::GetCurrentContext() || ImPlot::GetCurrentContext())
    return 3;
  return 0;
}
