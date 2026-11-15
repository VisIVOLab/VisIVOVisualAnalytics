#ifndef SolidSplats_h
#define SolidSplats_h

// Gaussian splats drawn in one colour (no colour column), issue #263.
//
// vtkPointGaussianMapper applies its scalar opacity function only to mapped
// scalars: with scalar visibility off every intensity drew the same, fully
// opaque picture. So solid splats keep their scalars, mapped through a
// one-colour table. The scalars left from a column coloured before may hold
// NaN, which a table maps to its NaN colour and the opacity function to an
// undefined opacity: they are set to 0, and the table's NaN colour is the
// solid colour too.
//
// Templates over the VTK types (vtkLookupTable, vtkDoubleArray) so the rule
// is tested without VTK (tests/TestSolidSplats.h).

namespace SolidSplats {

/** The one colour (the points' colour without a column, as the backend's). */
constexpr double kRed = 0.8, kGreen = 0.85, kBlue = 1.0;

/** A table that maps every value, NaN included, to the solid colour. */
template <class Lut>
void configureTable(Lut *lut)
{
    lut->SetNumberOfTableValues(2);
    lut->SetTableRange(-1.0e15, 1.0e15);
    for (int i = 0; i < 2; ++i)
        lut->SetTableValue(i, kRed, kGreen, kBlue, 1.0);
    lut->SetNanColor(kRed, kGreen, kBlue, 1.0);
}

/** The scalars of solid splats: every value 0 (no stale NaN). */
template <class Array>
void clearScalars(Array *scalars)
{
    auto n = scalars->GetNumberOfTuples();
    for (decltype(n) i = 0; i < n; ++i)
        scalars->SetValue(i, 0.0);
    scalars->Modified();
}

} // namespace SolidSplats

#endif
