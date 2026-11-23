#ifndef ParticleSplatVtk_h
#define ParticleSplatVtk_h

#include "ParticleSplatDefaults.h"

#include <vtkNew.h>
#include <vtkPiecewiseFunction.h>
#include <vtkPointGaussianMapper.h>
#include <vtkRenderStepsPass.h>
#include <vtkSmartPointer.h>
#include <vtkToneMappingPass.h>

#include <string>

// The VTK side of the Gaussian splats (issue #457), shared by the catalogue
// viewer and tools/splat_brightness.cpp so the validation draws what the
// viewer draws. Only API that VTK 9.3 already has.
namespace ParticleSplatVtk {

/// The renderer's passes drawn into a float32 target and clamped onto the
/// window: splats fainter than an 8-bit step add up there instead of being
/// rounded away at every addition, and whatever is within 0..1 (background,
/// axes, labels, the highlight spheres) is left as it was.
inline vtkSmartPointer<vtkToneMappingPass> floatTargetPass()
{
    auto tone = vtkSmartPointer<vtkToneMappingPass>::New();
    tone->SetToneMappingType(vtkToneMappingPass::Clamp);
    vtkNew<vtkRenderStepsPass> steps;
    tone->SetDelegatePass(steps);
    return tone;
}

/// An opacity function of 1: the splat's opacity is its fragment code's gain
/// alone, never the colour's 8-bit alpha.
inline void setUnitOpacity(vtkPiecewiseFunction *opacity)
{
    opacity->RemoveAllPoints();
    opacity->AddPoint(-1.0e15, 1.0);
    opacity->AddPoint(1.0e15, 1.0);
}

/// Gives the mapper the fragment code of a splat of peak opacity ``gain``.
/// The gain is a literal in the code, so a new one compiles a new program:
/// the code is set only when it changes. Returns whether it changed.
inline bool setSplatGain(vtkPointGaussianMapper *mapper, double gain)
{
    const std::string code = ParticleSplatDefaults::splatShaderCode(gain);
    if (code.empty())
        return false;
    const char *current = mapper->GetSplatShaderCode();
    if (current && code == current)
        return false;
    mapper->SetSplatShaderCode(code.c_str());
    return true;
}

} // namespace ParticleSplatVtk

#endif
