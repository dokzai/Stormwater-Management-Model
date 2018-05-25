//-----------------------------------------------------------------------------
//   gwater.c
//
//   Project:  EPA SWMM5
//   Version:  5.1
//   Date:     03/19/14  (Build 5.1.000)
//             09/15/14  (Build 5.1.007)
//             03/19/15  (Build 5.1.008)
//             08/05/15  (Build 5.1.010)
//   Author:   L. Rossman
//
//   Groundwater functions.
//
//   Build 5.1.007:
//   - User-supplied function for deep GW seepage flow added.
//   - New variable names for use in user-supplied GW flow equations added.
//
//   Build 5.1.008:
//   - More variable names for user-supplied GW flow equations added.
//   - Subcatchment area made into a shared variable.
//   - Evaporation loss initialized to 0.
//   - Support for collecting GW statistics added.
//
//   Build 5.1.010:
//   - Unsaturated hydraulic conductivity added to GW flow equation variables.
//
//-----------------------------------------------------------------------------
#define _CRT_SECURE_NO_DEPRECATE

#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "headers.h"
#include "odesolve.h"

//-----------------------------------------------------------------------------
//  Constants
//-----------------------------------------------------------------------------
static const double GWTOL = 0.0001;    // ODE solver tolerance
static const double XTOL  = 0.001;     // tolerance for moisture & depth

enum   GWstates {THETA,                // moisture content of upper GW zone
                 LOWERDEPTH};          // depth of lower saturated GW zone

enum   GWvariables {
	     gwvHGW,                       // water table height (ft)
         gwvHSW,                       // surface water height (ft)
         gwvHCB,                       // channel bottom height (ft)           //(5.1.007)
         gwvHGS,                       // ground surface height (ft)           //(5.1.007)
         gwvKS,                        // sat. hyd. condutivity (ft/s)         //(5.1.007)
         gwvK,                         // unsat. hyd. conductivity (ft/s)      //(5.1.008)
         gwvTHETA,                     // upper zone moisture content          //(5.1.008)
         gwvPHI,                       // soil porosity                        //(5.1.008)
         gwvFI,                        // surface infiltration (ft/s)          //(5.1.008)
         gwvFU,                        // uper zone percolation rate (ft/s)    //(5.1.008)
         gwvA,                         // subcatchment area (ft2)              //(5.1.008)
         gwvMAX};

// Names of GW variables that can be used in GW outflow expression
static const char* GWVarWords[] = {"HGW", "HSW", "HCB", "HGS", "KS", "K",            //(5.1.010)
                             "THETA", "PHI", "FI", "FU", "A", NULL};           //(5.1.008)

//-----------------------------------------------------------------------------
//  External Functions (declared in funcs.h)
//-----------------------------------------------------------------------------
//  gwater_readAquiferParams     (called by input_readLine)
//  gwater_readGroundwaterParams (called by input_readLine)
//  gwater_readFlowExpression    (called by input_readLine)
//  gwater_deleteFlowExpression  (called by deleteObjects in project.c)
//  gwater_validateAquifer       (called by swmm_open)
//  gwater_validate              (called by subcatch_validate) 
//  gwater_initState             (called by subcatch_initState)
//  gwater_getVolume             (called by massbal_open & massbal_getGwaterError)
//  gwater_getGroundwater        (called by getSubareaRunoff in subcatch.c)
//  gwater_getState              (called by saveRunoff in hotstart.c)
//  gwater_setState              (called by readRunoff in hotstart.c)

//-----------------------------------------------------------------------------
//  Local functions
//-----------------------------------------------------------------------------
static void   getDxDt(SWMM_Project *sp, double t, double* x, double* dxdt);
static void   getFluxes(SWMM_Project *sp, double upperVolume, double lowerDepth);
static void   getEvapRates(SWMM_Project *sp, double theta, double upperDepth);
static double getUpperPerc(SWMM_Project *sp, double theta, double upperDepth);
static double getGWFlow(SWMM_Project *sp, double lowerDepth);
static void   updateMassBal(SWMM_Project *sp, double area,  double tStep);

// Used to process custom GW outflow equations
static int    getVariableIndex(SWMM_Project *sp, char* s);
static double getVariableValue(SWMM_Project *sp, int varIndex);

//=============================================================================

int gwater_readAquiferParams(SWMM_Project *sp, int j, char* tok[], int ntoks)
//
//  Input:   j = aquifer index
//           tok[] = array of string tokens
//           ntoks = number of tokens
//  Output:  returns error message
//  Purpose: reads aquifer parameter values from line of input data
//
//  Data line contains following parameters:
//    ID, porosity, wiltingPoint, fieldCapacity,     conductivity,
//    conductSlope, tensionSlope, upperEvapFraction, lowerEvapDepth,
//    gwRecession,  bottomElev,   waterTableElev,    upperMoisture
//    (evapPattern)
//
{
    int   i, p;
    double x[12];
    char *id;

    // --- check that aquifer exists
    if ( ntoks < 13 ) return error_setInpError(sp, ERR_ITEMS, "");
    id = project_findID(sp, AQUIFER, tok[0]);
    if ( id == NULL ) return error_setInpError(sp, ERR_NAME, tok[0]);

    // --- read remaining tokens as numbers
    for (i = 0; i < 11; i++) x[i] = 0.0;
    for (i = 1; i < 13; i++)
    {
        if ( ! getDouble(tok[i], &x[i-1]) )
            return error_setInpError(sp, ERR_NUMBER, tok[i]);
    }

    // --- read upper evap pattern if present
    p = -1;
    if ( ntoks > 13 )
    {
        p = project_findObject(sp, TIMEPATTERN, tok[13]);
        if ( p < 0 ) return error_setInpError(sp, ERR_NAME, tok[13]);
    }

    // --- assign parameters to aquifer object
    sp->Aquifer[j].ID = id;
    sp->Aquifer[j].porosity       = x[0];
    sp->Aquifer[j].wiltingPoint   = x[1];
    sp->Aquifer[j].fieldCapacity  = x[2];
    sp->Aquifer[j].conductivity   = x[3] / UCF(sp, RAINFALL);
    sp->Aquifer[j].conductSlope   = x[4];
    sp->Aquifer[j].tensionSlope   = x[5] / UCF(sp, LENGTH);
    sp->Aquifer[j].upperEvapFrac  = x[6];
    sp->Aquifer[j].lowerEvapDepth = x[7] / UCF(sp, LENGTH);
    sp->Aquifer[j].lowerLossCoeff = x[8] / UCF(sp, RAINFALL);
    sp->Aquifer[j].bottomElev     = x[9] / UCF(sp, LENGTH);
    sp->Aquifer[j].waterTableElev = x[10] / UCF(sp, LENGTH);
    sp->Aquifer[j].upperMoisture  = x[11];
    sp->Aquifer[j].upperEvapPat   = p;
    return 0;
}

//=============================================================================

int gwater_readGroundwaterParams(SWMM_Project *sp, char* tok[], int ntoks)
//
//  Input:   tok[] = array of string tokens
//           ntoks = number of tokens
//  Output:  returns error code
//  Purpose: reads groundwater inflow parameters for a subcatchment from
//           a line of input data.
//
//  Data format is:
//  subcatch  aquifer  node  surfElev  a1  b1  a2  b2  a3  fixedDepth +
//            (nodeElev  bottomElev  waterTableElev  upperMoisture )
//
{
    int    i, j, k, m, n;
    double x[11];
    TGroundwater* gw;

    // --- check that specified subcatchment, aquifer & node exist
    if ( ntoks < 3 ) return error_setInpError(sp, ERR_ITEMS, "");
    j = project_findObject(sp, SUBCATCH, tok[0]);
    if ( j < 0 ) return error_setInpError(sp, ERR_NAME, tok[0]);

    // --- check for enough tokens
    if ( ntoks < 11 ) return error_setInpError(sp, ERR_ITEMS, "");

    // --- check that specified aquifer and node exists
    k = project_findObject(sp, AQUIFER, tok[1]);
    if ( k < 0 ) return error_setInpError(sp, ERR_NAME, tok[1]);
    n = project_findObject(sp, NODE, tok[2]);
    if ( n < 0 ) return error_setInpError(sp, ERR_NAME, tok[2]);

    // -- read in the flow parameters
    for ( i = 0; i < 7; i++ )
    {
        if ( ! getDouble(tok[i+3], &x[i]) ) 
            return error_setInpError(sp, ERR_NUMBER, tok[i+3]);
    }

    // --- read in optional depth parameters
    for ( i = 7; i < 11; i++)
    {
        x[i] = MISSING;
        m = i + 3;
        if ( ntoks > m && *tok[m] != '*' )
        {    
            if (! getDouble(tok[m], &x[i]) ) 
                return error_setInpError(sp, ERR_NUMBER, tok[m]);
            if ( i < 10 ) x[i] /= UCF(sp, LENGTH);
        }
    }

    // --- create a groundwater flow object
    if ( !sp->Subcatch[j].groundwater )
    {
        gw = (TGroundwater *) malloc(sizeof(TGroundwater));
        if ( !gw ) return error_setInpError(sp, ERR_MEMORY, "");
        sp->Subcatch[j].groundwater = gw;
    }
    else gw = sp->Subcatch[j].groundwater;

    // --- populate the groundwater flow object with its parameters
    gw->aquifer    = k;
    gw->node       = n;
    gw->surfElev   = x[0] / UCF(sp, LENGTH);
    gw->a1         = x[1];
    gw->b1         = x[2];
    gw->a2         = x[3];
    gw->b2         = x[4];
    gw->a3         = x[5];
    gw->fixedDepth = x[6] / UCF(sp, LENGTH);
    gw->nodeElev   = x[7];                       //already converted to ft.
    gw->bottomElev     = x[8];
    gw->waterTableElev = x[9];
    gw->upperMoisture  = x[10];
    return 0;
}

//=============================================================================

////  This function was re-written for release 5.1.007.  ////                  //(5.1.007)

int gwater_readFlowExpression(SWMM_Project *sp, char* tok[], int ntoks)
//
//  Input:   tok[] = array of string tokens
//           ntoks = number of tokens
//  Output:  returns error code
//  Purpose: reads mathematical expression for lateral or deep groundwater
//           flow for a subcatchment from a line of input data.
//
//  Format is: subcatch LATERAL/DEEP <expr>
//     where subcatch is the ID of the subcatchment, LATERAL is for lateral
//     GW flow, DEEP is for deep GW flow and <expr> is any well-formed math
//     expression. 
//
{
    int   i, j, k;
    char  exprStr[MAXLINE+1];
    MathExpr* expr;

    // --- return if too few tokens
    if ( ntoks < 3 ) return error_setInpError(sp, ERR_ITEMS, "");

    // --- check that subcatchment exists
    j = project_findObject(sp, SUBCATCH, tok[0]);
    if ( j < 0 ) return error_setInpError(sp, ERR_NAME, tok[0]);

    // --- check if expression is for lateral or deep GW flow
    k = 1;
    if ( match(tok[1], "LAT") ) k = 1;
    else if ( match(tok[1], "DEEP") ) k = 2;
    else return error_setInpError(sp, ERR_KEYWORD, tok[1]);

    // --- concatenate remaining tokens into a single string
    strcpy(exprStr, tok[2]);
    for ( i = 3; i < ntoks; i++)
    {
        strcat(exprStr, " ");
        strcat(exprStr, tok[i]);
    }

    // --- delete any previous flow eqn.
    if ( k == 1 ) mathexpr_delete(sp->Subcatch[j].gwLatFlowExpr);
    else          mathexpr_delete(sp->Subcatch[j].gwDeepFlowExpr);

    // --- create a parsed expression tree from the string expr
    //     (getVariableIndex is the function that converts a GW
    //      variable's name into an index number) 
    expr = mathexpr_create(sp, exprStr, getVariableIndex);
    if ( expr == NULL ) return error_setInpError(sp, ERR_TREATMENT_EXPR, "");

    // --- save expression tree with the subcatchment
    if ( k == 1 ) sp->Subcatch[j].gwLatFlowExpr = expr;
    else          sp->Subcatch[j].gwDeepFlowExpr = expr;
    return 0;
}

//=============================================================================

void gwater_deleteFlowExpression(SWMM_Project *sp, int j)
//
//  Input:   j = subcatchment index
//  Output:  none
//  Purpose: deletes a subcatchment's custom groundwater flow expressions.     //(5.1.007)
//
{
    mathexpr_delete(sp->Subcatch[j].gwLatFlowExpr);
    mathexpr_delete(sp->Subcatch[j].gwDeepFlowExpr);
}

//=============================================================================

void  gwater_validateAquifer(SWMM_Project *sp, int j)
//
//  Input:   j = aquifer index
//  Output:  none
//  Purpose: validates groundwater aquifer properties .
//
{
	int p;

    if ( sp->Aquifer[j].porosity          <= 0.0 
    ||   sp->Aquifer[j].fieldCapacity     >= sp->Aquifer[j].porosity
    ||   sp->Aquifer[j].wiltingPoint      >= sp->Aquifer[j].fieldCapacity
    ||   sp->Aquifer[j].conductivity      <= 0.0
    ||   sp->Aquifer[j].conductSlope      <  0.0
    ||   sp->Aquifer[j].tensionSlope      <  0.0
    ||   sp->Aquifer[j].upperEvapFrac     <  0.0
    ||   sp->Aquifer[j].lowerEvapDepth    <  0.0
    ||   sp->Aquifer[j].waterTableElev    <  sp->Aquifer[j].bottomElev
    ||   sp->Aquifer[j].upperMoisture     >  sp->Aquifer[j].porosity 
    ||   sp->Aquifer[j].upperMoisture     <  sp->Aquifer[j].wiltingPoint )
        report_writeErrorMsg(sp, ERR_AQUIFER_PARAMS, sp->Aquifer[j].ID);

    p = sp->Aquifer[j].upperEvapPat;
    if ( p >= 0 && sp->Pattern[p].type != MONTHLY_PATTERN )
    {
        report_writeErrorMsg(sp, ERR_AQUIFER_PARAMS, sp->Aquifer[j].ID);
    }
}

//=============================================================================

void  gwater_validate(SWMM_Project *sp, int j)
{
    TAquifer a;         // Aquifer data structure
    TGroundwater* gw;   // Groundwater data structure
    
    gw = sp->Subcatch[j].groundwater;
    if ( gw )
    {
        a = sp->Aquifer[gw->aquifer];

        // ... use aquifer values for missing groundwater parameters
        if ( gw->bottomElev == MISSING ) gw->bottomElev = a.bottomElev;
        if ( gw->waterTableElev == MISSING ) gw->waterTableElev = a.waterTableElev;
        if ( gw->upperMoisture == MISSING ) gw->upperMoisture = a.upperMoisture;

        // ... ground elevation can't be below water table elevation
        if ( gw->surfElev < gw->waterTableElev )
            report_writeErrorMsg(sp, ERR_GROUND_ELEV, sp->Subcatch[j].ID);
    }
}

//=============================================================================

void  gwater_initState(SWMM_Project *sp, int j)
//
//  Input:   j = subcatchment index
//  Output:  none
//  Purpose: initializes state of subcatchment's groundwater.
//
{
    TAquifer a;         // Aquifer data structure
    TGroundwater* gw;   // Groundwater data structure
    
    gw = sp->Subcatch[j].groundwater;
    if ( gw )
    {
        a = sp->Aquifer[gw->aquifer];

        // ... initial moisture content
        gw->theta = gw->upperMoisture;
        if ( gw->theta >= a.porosity )
        {
            gw->theta = a.porosity - XTOL;
        }

        // ... initial depth of lower (saturated) zone
        gw->lowerDepth = gw->waterTableElev - gw->bottomElev;
        if ( gw->lowerDepth >= gw->surfElev - gw->bottomElev )
        {
            gw->lowerDepth = gw->surfElev - gw->bottomElev - XTOL;
        }

        // ... initial lateral groundwater outflow
        gw->oldFlow = 0.0;
        gw->newFlow = 0.0;
        gw->evapLoss = 0.0;                                                    //(5.1.008)

        // ... initial available infiltration volume into upper zone
        gw->maxInfilVol = (gw->surfElev - gw->waterTableElev) *
                          (a.porosity - gw->theta) /
                          subcatch_getFracPerv(sp, j);
    }
}

//=============================================================================

void gwater_getState(SWMM_Project *sp, int j, double x[])
//
//  Input:   j = subcatchment index
//  Output:  x[] = array of groundwater state variables
//  Purpose: retrieves state of subcatchment's groundwater.
//
{
    TGroundwater* gw = sp->Subcatch[j].groundwater;
    x[0] = gw->theta;
    x[1] = gw->bottomElev + gw->lowerDepth;
    x[2] = gw->newFlow;
    x[3] = gw->maxInfilVol;
}

//=============================================================================

void gwater_setState(SWMM_Project *sp, int j, double x[])
//
//  Input:   j = subcatchment index
//           x[] = array of groundwater state variables
//  Purpose: assigns values to a subcatchment's groundwater state.
//
{
    TGroundwater* gw = sp->Subcatch[j].groundwater;
    if ( gw == NULL ) return;

    gw->theta = x[0];
    gw->lowerDepth = x[1] - gw->bottomElev;
    gw->oldFlow = x[2];
    if ( x[3] != MISSING ) gw->maxInfilVol = x[3];
}

//=============================================================================

double gwater_getVolume(SWMM_Project *sp, int j)
//
//  Input:   j = subcatchment index
//  Output:  returns total volume of groundwater in ft/ft2
//  Purpose: finds volume of groundwater stored in upper & lower zones
//
{
    TAquifer a;
    TGroundwater* gw;
    double upperDepth;
    gw = sp->Subcatch[j].groundwater;
    if ( gw == NULL ) return 0.0;
    a = sp->Aquifer[gw->aquifer];
    upperDepth = gw->surfElev - gw->bottomElev - gw->lowerDepth;
    return (upperDepth * gw->theta) + (gw->lowerDepth * a.porosity);
}

//=============================================================================

void gwater_getGroundwater(SWMM_Project *sp, int j, double evap, double infil,
        double tStep)
//
//  Purpose: computes groundwater flow from subcatchment during current time step.
//  Input:   j     = subcatchment index
//           evap  = pervious surface evaporation volume consumed (ft3)
//           infil = surface infiltration volume (ft3)
//           tStep = time step (sec)
//  Output:  none
//

//  Note: local "area" variable was replaced with shared variable "Area". //   //(5.1.008)

{
    int    n;                          // node exchanging groundwater
    double x[2];                       // upper moisture content & lower depth 
    double vUpper;                     // upper vol. available for percolation
    double nodeFlow;                   // max. possible GW flow from node

    TGwaterShared *gwtr = &sp->GwaterShared;

    // --- save subcatchment's groundwater and aquifer objects to 
    //     shared variables
    gwtr->GW = sp->Subcatch[j].groundwater;
    if ( gwtr->GW == NULL ) return;
    gwtr->LatFlowExpr = sp->Subcatch[j].gwLatFlowExpr;                                   //(5.1.007)
    gwtr->DeepFlowExpr = sp->Subcatch[j].gwDeepFlowExpr;                                 //(5.1.007)
    gwtr->A = sp->Aquifer[gwtr->GW->aquifer];

    // --- get fraction of total area that is pervious
    gwtr->FracPerv = subcatch_getFracPerv(sp, j);
    if ( gwtr->FracPerv <= 0.0 ) return;
    gwtr->Area = sp->Subcatch[j].area;

    // --- convert infiltration volume (ft3) to equivalent rate
    //     over entire GW (subcatchment) area
    infil = infil / gwtr->Area / tStep;
    gwtr->Infil = infil;
    gwtr->Tstep = tStep;

    // --- convert pervious surface evaporation already exerted (ft3)
    //     to equivalent rate over entire GW (subcatchment) area
    evap = evap / gwtr->Area / tStep;

    // --- convert max. surface evap rate (ft/sec) to a rate
    //     that applies to GW evap (GW evap can only occur
    //     through the pervious land surface area)
    gwtr->MaxEvap = sp->Evap.rate * gwtr->FracPerv;

    // --- available subsurface evaporation is difference between max.
    //     rate and pervious surface evap already exerted
    gwtr->AvailEvap = MAX((gwtr->MaxEvap - evap), 0.0);

    // --- save total depth & outlet node properties to shared variables
    gwtr->TotalDepth = gwtr->GW->surfElev - gwtr->GW->bottomElev;
    if ( gwtr->TotalDepth <= 0.0 ) return;
    n = gwtr->GW->node;

    // --- establish min. water table height above aquifer bottom at which
    //     GW flow can occur (override node's invert if a value was provided
    //     in the GW object)
    if ( gwtr->GW->nodeElev != MISSING )
        gwtr->Hstar = gwtr->GW->nodeElev - gwtr->GW->bottomElev;
    else
        gwtr->Hstar = sp->Node[n].invertElev - gwtr->GW->bottomElev;
    
    // --- establish surface water height (relative to aquifer bottom)
    //     for drainage system node connected to the GW aquifer
    if ( gwtr->GW->fixedDepth > 0.0 )
    {
        gwtr->Hsw = gwtr->GW->fixedDepth + sp->Node[n].invertElev -
                gwtr->GW->bottomElev;
    }
    else
        gwtr->Hsw = sp->Node[n].newDepth + sp->Node[n].invertElev -
        gwtr->GW->bottomElev;

    // --- store state variables (upper zone moisture content, lower zone
    //     depth) in work vector x
    x[THETA] = gwtr->GW->theta;
    x[LOWERDEPTH] = gwtr->GW->lowerDepth;

    // --- set limit on percolation rate from upper to lower GW zone
    vUpper = (gwtr->TotalDepth - x[LOWERDEPTH]) * (x[THETA] - gwtr->A.fieldCapacity);
    vUpper = MAX(0.0, vUpper); 
    gwtr->MaxUpperPerc = vUpper / tStep;

    // --- set limit on GW flow out of aquifer based on volume of lower zone
    gwtr->MaxGWFlowPos = x[LOWERDEPTH]*gwtr->A.porosity / tStep;

    // --- set limit on GW flow into aquifer from drainage system node
    //     based on min. of capacity of upper zone and drainage system
    //     inflow to the node
    gwtr->MaxGWFlowNeg = (gwtr->TotalDepth - x[LOWERDEPTH]) *
            (gwtr->A.porosity - x[THETA]) / tStep;
    nodeFlow = (sp->Node[n].inflow + sp->Node[n].newVolume/tStep) / gwtr->Area;
    gwtr->MaxGWFlowNeg = -MIN(gwtr->MaxGWFlowNeg, nodeFlow);
    
    // --- integrate eqns. for d(Theta)/dt and d(LowerDepth)/dt
    //     NOTE: ODE solver must have been initialized previously
    odesolve_integrate(sp, x, 2, 0, tStep, GWTOL, tStep, getDxDt);
    
    // --- keep state variables within allowable bounds
    x[THETA] = MAX(x[THETA], gwtr->A.wiltingPoint);
    if ( x[THETA] >= gwtr->A.porosity )
    {
        x[THETA] = gwtr->A.porosity - XTOL;
        x[LOWERDEPTH] = gwtr->TotalDepth - XTOL;
    }
    x[LOWERDEPTH] = MAX(x[LOWERDEPTH],  0.0);
    if ( x[LOWERDEPTH] >= gwtr->TotalDepth )
    {
        x[LOWERDEPTH] = gwtr->TotalDepth - XTOL;
    }

    // --- save new values of state values
    gwtr->GW->theta = x[THETA];
    gwtr->GW->lowerDepth  = x[LOWERDEPTH];
    getFluxes(sp, gwtr->GW->theta, gwtr->GW->lowerDepth);
    gwtr->GW->oldFlow = gwtr->GW->newFlow;
    gwtr->GW->newFlow = gwtr->GWFlow;
    gwtr->GW->evapLoss = gwtr->UpperEvap + gwtr->LowerEvap;

    //--- find max. infiltration volume (as depth over
    //    the pervious portion of the subcatchment)
    //    that upper zone can support in next time step
    gwtr->GW->maxInfilVol = (gwtr->TotalDepth - x[LOWERDEPTH]) *
                      (gwtr->A.porosity - x[THETA]) / gwtr->FracPerv;

    // --- update GW mass balance
    updateMassBal(sp, gwtr->Area, tStep);

    // --- update GW statistics                                                //(5.1.008)
    stats_updateGwaterStats(sp, j, infil, gwtr->GW->evapLoss, gwtr->GWFlow,
            gwtr->LowerLoss, gwtr->GW->theta, gwtr->GW->lowerDepth +
            gwtr->GW->bottomElev, tStep);                    //(5.1.008)
}

//=============================================================================

void updateMassBal(SWMM_Project *sp, double area, double tStep)
//
//  Input:   area  = subcatchment area (ft2)
//           tStep = time step (sec)
//  Output:  none
//  Purpose: updates GW mass balance with volumes of water fluxes.
//
{
    double vInfil;                     // infiltration volume
    double vUpperEvap;                 // upper zone evap. volume
    double vLowerEvap;                 // lower zone evap. volume
    double vLowerPerc;                 // lower zone deep perc. volume
    double vGwater;                    // volume of exchanged groundwater
    double ft2sec = area * tStep;

    TGwaterShared *gwtr = &sp->GwaterShared;

    vInfil     = gwtr->Infil * ft2sec;
    vUpperEvap = gwtr->UpperEvap * ft2sec;
    vLowerEvap = gwtr->LowerEvap * ft2sec;
    vLowerPerc = gwtr->LowerLoss * ft2sec;
    vGwater    = 0.5 * (gwtr->GW->oldFlow + gwtr->GW->newFlow) * ft2sec;
    massbal_updateGwaterTotals(sp, vInfil, vUpperEvap, vLowerEvap, vLowerPerc,
                               vGwater);
}

//=============================================================================

////  This function was re-written for release 5.1.007.  ////                  //(5.1.007)

void  getFluxes(SWMM_Project *sp, double theta, double lowerDepth)
//
//  Input:   upperVolume = vol. depth of upper zone (ft)
//           upperDepth  = depth of upper zone (ft)
//  Output:  none
//  Purpose: computes water fluxes into/out of upper/lower GW zones.
//
{
    double upperDepth;

    TGwaterShared *gwtr = &sp->GwaterShared;

    // --- find upper zone depth
    lowerDepth = MAX(lowerDepth, 0.0);
    lowerDepth = MIN(lowerDepth, gwtr->TotalDepth);
    upperDepth = gwtr->TotalDepth - lowerDepth;

    // --- save lower depth and theta to global variables
    gwtr->Hgw = lowerDepth;
    gwtr->Theta = theta;

    // --- find evaporation rate from both zones
    getEvapRates(sp, theta, upperDepth);

    // --- find percolation rate from upper to lower zone
    gwtr->UpperPerc = getUpperPerc(sp, theta, upperDepth);
    gwtr->UpperPerc = MIN(gwtr->UpperPerc, gwtr->MaxUpperPerc);

    // --- find loss rate to deep GW
    if ( gwtr->DeepFlowExpr != NULL )
        gwtr->LowerLoss = mathexpr_eval(sp, gwtr->DeepFlowExpr, getVariableValue) /
                    UCF(sp, RAINFALL);
    else
        gwtr->LowerLoss = gwtr->A.lowerLossCoeff * lowerDepth / gwtr->TotalDepth;
    gwtr->LowerLoss = MIN(gwtr->LowerLoss, lowerDepth/gwtr->Tstep);

    // --- find GW flow rate from lower zone to drainage system node
    gwtr->GWFlow = getGWFlow(sp, lowerDepth);
    if ( gwtr->LatFlowExpr != NULL )
    {
        gwtr->GWFlow += mathexpr_eval(sp, gwtr->LatFlowExpr, getVariableValue) /
                UCF(sp, GWFLOW);
    }
    if ( gwtr->GWFlow >= 0.0 )
        gwtr->GWFlow = MIN(gwtr->GWFlow, gwtr->MaxGWFlowPos);
    else
        gwtr->GWFlow = MAX(gwtr->GWFlow, gwtr->MaxGWFlowNeg);
}

//=============================================================================

void  getDxDt(SWMM_Project *sp, double t, double* x, double* dxdt)
//
//  Input:   t    = current time (not used)
//           x    = array of state variables
//  Output:  dxdt = array of time derivatives of state variables
//  Purpose: computes time derivatives of upper moisture content 
//           and lower depth.
//
{
    double qUpper;    // inflow - outflow for upper zone (ft/sec)
    double qLower;    // inflow - outflow for lower zone (ft/sec)
    double denom;

    TGwaterShared *gwtr = &sp->GwaterShared;

    getFluxes(sp, x[THETA], x[LOWERDEPTH]);
    qUpper = gwtr->Infil - gwtr->UpperEvap - gwtr->UpperPerc;
    qLower = gwtr->UpperPerc - gwtr->LowerLoss - gwtr->LowerEvap - gwtr->GWFlow;

    // --- d(upper zone moisture)/dt = (net upper zone flow) /
    //                                 (upper zone depth)
    denom = gwtr->TotalDepth - x[LOWERDEPTH];
    if (denom > 0.0)
        dxdt[THETA] = qUpper / denom;
    else
        dxdt[THETA] = 0.0;

    // --- d(lower zone depth)/dt = (net lower zone flow) /
    //                              (upper zone moisture deficit)
    denom = gwtr->A.porosity - x[THETA];
    if (denom > 0.0)
        dxdt[LOWERDEPTH] = qLower / denom;
    else
        dxdt[LOWERDEPTH] = 0.0;
}

//=============================================================================

void getEvapRates(SWMM_Project *sp, double theta, double upperDepth)
//
//  Input:   theta      = moisture content of upper zone
//           upperDepth = depth of upper zone (ft)
//  Output:  none
//  Purpose: computes evapotranspiration out of upper & lower zones.
//
{
    int    p, month;
    double f;
    double lowerFrac, upperFrac;

    TGwaterShared *gwtr = &sp->GwaterShared;

    // --- no GW evaporation when infiltration is occurring
    gwtr->UpperEvap = 0.0;
    gwtr->LowerEvap = 0.0;
    if ( gwtr->Infil > 0.0 ) return;

    // --- get monthly-adjusted upper zone evap fraction
    upperFrac = gwtr->A.upperEvapFrac;
    f = 1.0;
    p = gwtr->A.upperEvapPat;
    if ( p >= 0 )
    {
        month = datetime_monthOfYear(getDateTime(sp, sp->NewRunoffTime));
        f = sp->Pattern[p].factor[month-1];
    }
    upperFrac *= f;

    // --- upper zone evaporation requires that soil moisture
    //     be above the wilting point
    if ( theta > gwtr->A.wiltingPoint )
    {
        // --- actual evap is upper zone fraction applied to max. potential
        //     rate, limited by the available rate after any surface evap 
        gwtr->UpperEvap = upperFrac * gwtr->MaxEvap;
        gwtr->UpperEvap = MIN(gwtr->UpperEvap, gwtr->AvailEvap);
    }

    // --- check if lower zone evaporation is possible
    if ( gwtr->A.lowerEvapDepth > 0.0 )
    {
        // --- find the fraction of the lower evaporation depth that
        //     extends into the saturated lower zone
        lowerFrac = (gwtr->A.lowerEvapDepth - upperDepth) / gwtr->A.lowerEvapDepth;
        lowerFrac = MAX(0.0, lowerFrac);
        lowerFrac = MIN(lowerFrac, 1.0);

        // --- make the lower zone evap rate proportional to this fraction
        //     and the evap not used in the upper zone
        gwtr->LowerEvap = lowerFrac * (1.0 - upperFrac) * gwtr->MaxEvap;
        gwtr->LowerEvap = MIN(gwtr->LowerEvap, (gwtr->AvailEvap - gwtr->UpperEvap));
    }
}

//=============================================================================

double getUpperPerc(SWMM_Project *sp, double theta, double upperDepth)
//
//  Input:   theta      = moisture content of upper zone
//           upperDepth = depth of upper zone (ft)
//  Output:  returns percolation rate (ft/sec)
//  Purpose: finds percolation rate from upper to lower zone.
//
{
    double delta;                       // unfilled water content of upper zone
    double dhdz;                        // avg. change in head with depth
    double hydcon;                      // unsaturated hydraulic conductivity

    TGwaterShared *gwtr = &sp->GwaterShared;

    // --- no perc. from upper zone if no depth or moisture content too low    
    if ( upperDepth <= 0.0 || theta <= gwtr->A.fieldCapacity ) return 0.0;

    // --- compute hyd. conductivity as function of moisture content
    delta = theta - gwtr->A.porosity;
    hydcon = gwtr->A.conductivity * exp(delta * gwtr->A.conductSlope);

    // --- compute integral of dh/dz term
    delta = theta - gwtr->A.fieldCapacity;
    dhdz = 1.0 + gwtr->A.tensionSlope * 2.0 * delta / upperDepth;

    // --- compute upper zone percolation rate
    gwtr->HydCon = hydcon;                                                           //(5.1.010)
    return hydcon * dhdz;
}

//=============================================================================

double getGWFlow(SWMM_Project *sp, double lowerDepth)
//
//  Input:   lowerDepth = depth of lower zone (ft)
//  Output:  returns groundwater flow rate (ft/sec)
//  Purpose: finds groundwater outflow from lower saturated zone.
//
{
    double q, t1, t2, t3;

    TGwaterShared *gwtr = &sp->GwaterShared;

    // --- water table must be above Hstar for flow to occur
    if ( lowerDepth <= gwtr->Hstar ) return 0.0;

    // --- compute groundwater component of flow
    if ( gwtr->GW->b1 == 0.0 )
        t1 = gwtr->GW->a1;
    else
        t1 = gwtr->GW->a1 * pow( (lowerDepth - gwtr->Hstar)*UCF(sp, LENGTH), gwtr->GW->b1);

    // --- compute surface water component of flow
    if ( gwtr->GW->b2 == 0.0 ) t2 = gwtr->GW->a2;
    else if (gwtr->Hsw > gwtr->Hstar)
    {
        t2 = gwtr->GW->a2 * pow( (gwtr->Hsw - gwtr->Hstar)*UCF(sp, LENGTH), gwtr->GW->b2);
    }
    else t2 = 0.0;

    // --- compute groundwater/surface water interaction term
    t3 = gwtr->GW->a3 * lowerDepth * gwtr->Hsw * UCF(sp, LENGTH) * UCF(sp, LENGTH);

    // --- compute total groundwater flow
    q = (t1 - t2 + t3) / UCF(sp, GWFLOW); 
    if ( q < 0.0 && gwtr->GW->a3 != 0.0 ) q = 0.0;
    return q;
}

//=============================================================================

int  getVariableIndex(SWMM_Project *sp, char* s)
//
//  Input:   s = name of a groundwater variable
//  Output:  returns index of groundwater variable
//  Purpose: finds position of GW variable in list of GW variable names.
//
{
    int k;

    k = findmatch(s, GWVarWords);
    if ( k >= 0 ) return k;
    return -1;
}

//=============================================================================

double getVariableValue(SWMM_Project *sp, int varIndex)
//
//  Input:   varIndex = index of a GW variable
//  Output:  returns current value of GW variable
//  Purpose: finds current value of a GW variable.
//
{
    TGwaterShared *gwtr = &sp->GwaterShared;

    switch (varIndex)
    {
    case gwvHGW:  return gwtr->Hgw * UCF(sp, LENGTH);
    case gwvHSW:  return gwtr->Hsw * UCF(sp, LENGTH);
    case gwvHCB:  return gwtr->Hstar * UCF(sp, LENGTH);
    case gwvHGS:  return gwtr->TotalDepth * UCF(sp, LENGTH);
    case gwvKS:   return gwtr->A.conductivity * UCF(sp, RAINFALL);
    case gwvK:    return gwtr->HydCon * UCF(sp, RAINFALL);                               //(5.1.010)
    case gwvTHETA:return gwtr->Theta;                                                //(5.1.008)
    case gwvPHI:  return gwtr->A.porosity;                                           //(5.1.008)
    case gwvFI:   return gwtr->Infil * UCF(sp, RAINFALL);                                //(5.1.008)
    case gwvFU:   return gwtr->UpperPerc * UCF(sp, RAINFALL);                            //(5.1.008)
    case gwvA:    return gwtr->Area * UCF(sp, LANDAREA);                                 //(5.1.008)
    default:      return 0.0;
    }
}