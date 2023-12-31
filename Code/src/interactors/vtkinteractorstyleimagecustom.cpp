#include "vtkinteractorstyleimagecustom.h"

#include "../astroutils.h"

#include <vtkImageProperty.h>
#include <vtkObjectFactory.h>
#include <vtkRenderer.h>
#include <vtkRendererCollection.h>
#include <vtkRenderWindow.h>
#include <vtkRenderWindowInteractor.h>

#include <sstream>

#include <QDebug>

vtkStandardNewMacro(vtkInteractorStyleImageCustom);

//----------------------------------------------------------------------------
vtkInteractorStyleImageCustom::vtkInteractorStyleImageCustom()
{
    this->Coordinate = vtkSmartPointer<vtkCoordinate>::New();
    this->Coordinate->SetCoordinateSystemToDisplay();
    this->CurrentLayerFitsReader = NULL;
    this->CurrentLayerFitsHeader = "";

    this->zComp = []() -> int { return 0; };
}

//----------------------------------------------------------------------------
void vtkInteractorStyleImageCustom::PrintSelf(std::ostream &os, vtkIndent indent)
{
    Superclass::PrintSelf(os, indent);
    os << indent << "Current FitsReader: " << CurrentLayerFitsReader()->GetFileName() << endl;
}

//----------------------------------------------------------------------------
void vtkInteractorStyleImageCustom::OnMouseMove()
{
    if (this->CurrentImageProperty) {
        vtkImageProperty *property = this->CurrentImageProperty;

        if (ColorWindow.find(property) == ColorWindow.end()) {
            ColorWindow[property] = property->GetColorWindow();
        }

        if (ColorLevel.find(property) == ColorLevel.end()) {
            ColorLevel[property] = property->GetColorLevel();
        }
    }

    Coordinate->SetValue(this->GetInteractor()->GetEventPosition()[0],
                         this->GetInteractor()->GetEventPosition()[1], 0);

    double *world_coord = Coordinate->GetComputedWorldValue(
            this->GetInteractor()->GetRenderWindow()->GetRenderers()->GetFirstRenderer());

    double sky_coord[2];
    double sky_coord_gal[2];
    double sky_coord_fk5[2];

    std::ostringstream oss;
    vtkSmartPointer<vtkFitsReader> FitsReader = NULL;
    // Pixel value and coords
    if (this->CurrentLayerFitsReader != NULL)
        FitsReader = CurrentLayerFitsReader();
    /*
       float *pixel = static_cast<float *>(
               FitsReader->GetOutput()->GetScalarPointer(world_coord[0], world_coord[1], zComp()));
       oss << "<value> ";
       if (pixel != NULL)
           oss << pixel[0];
       else
           oss << "NaN";
    */
    std::string filename;
    oss << " <image> X: " << world_coord[0] << " Y: " << world_coord[1];
    if (FitsReader != NULL)
        filename = FitsReader->GetFileName();
    else
        filename = this->CurrentLayerFitsHeader;
    // Galactic coords
    AstroUtils::xy2sky(filename, world_coord[0], world_coord[1], sky_coord_gal, WCS_GALACTIC);
    oss << " <galactic> GLON: " << sky_coord_gal[0] << " GLAT: " << sky_coord_gal[1];

    // fk5 coords
    AstroUtils::xy2sky(filename, world_coord[0], world_coord[1], sky_coord_fk5, WCS_J2000);
    oss << " <fk5> RA: " << sky_coord_fk5[0] << " DEC: " << sky_coord_fk5[1];

    // Ecliptic coords
    AstroUtils::xy2sky(filename, world_coord[0], world_coord[1], sky_coord);
    oss << " <ecliptic> RA: " << sky_coord[0] << " DEC: " << sky_coord[1];

    CoordsCallback(oss.str());

    Superclass::OnMouseMove();
}

//----------------------------------------------------------------------------
void vtkInteractorStyleImageCustom::OnChar()
{
    vtkRenderWindowInteractor *rwi = this->Interactor;

    switch (rwi->GetKeyCode()) {
    case 'r':
    case 'R':
        // Allow either shift/ctrl to trigger the usual 'r' binding
        // otherwise trigger reset window level event
        if (rwi->GetShiftKey() || rwi->GetControlKey()) {
            Superclass::OnChar();
        } else if (this->HandleObservers && this->HasObserver(vtkCommand::ResetWindowLevelEvent)) {
            this->InvokeEvent(vtkCommand::ResetWindowLevelEvent, this);
        } else if (this->CurrentImageProperty) {
            vtkImageProperty *property = this->CurrentImageProperty;

            property->SetColorLevel(ColorLevel[property]);
            property->SetColorWindow(ColorWindow[property]);

            this->Interactor->Render();
        }
        break;
    }
}

//----------------------------------------------------------------------------
void vtkInteractorStyleImageCustom::SetCoordsCallback(
        const std::function<void(std::string)> &callback)
{
    this->CoordsCallback = callback;
}

//----------------------------------------------------------------------------
void vtkInteractorStyleImageCustom::SetLayerFitsReaderFunc(
        const std::function<vtkSmartPointer<vtkFitsReader>()> &callback)
{
    this->CurrentLayerFitsReader = callback;
}

//----------------------------------------------------------------------------
void vtkInteractorStyleImageCustom::SetLayerFitsReaderFunc(const std::string &fn)
{
    this->CurrentLayerFitsHeader = fn;
}

//----------------------------------------------------------------------------
void vtkInteractorStyleImageCustom::SetPixelZCompFunc(const std::function<int()> &callback)
{
    this->zComp = callback;
}
