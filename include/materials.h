/**
 * @file materials.h
 * @brief Material Database for RPM Calculation
 *
 * Surface speeds and RPM calculation for drilling different materials.
 */

#ifndef MATERIALS_H
#define MATERIALS_H

#include <stdint.h>

/*===========================================================================*/
/* Material Types                                                            */
/*===========================================================================*/

typedef enum {
    MATERIAL_SOFTWOOD = 0,
    MATERIAL_HARDWOOD,
    MATERIAL_PLYWOOD,
    MATERIAL_MDF,
    MATERIAL_ALUMINUM,
    MATERIAL_BRASS,
    MATERIAL_STEEL,
    MATERIAL_STAINLESS,
    MATERIAL_ACRYLIC,
    MATERIAL_ABS,
    MATERIAL_PVC,
    MATERIAL_PLASTIC,
    MATERIAL_COUNT
} material_type_t;

/*===========================================================================*/
/* Bit Types                                                                 */
/*===========================================================================*/

typedef enum {
    BIT_TWIST = 0,      // Standard twist drill (baseline 1.0x)
    BIT_BRAD,           // Brad point (1.0x - centers better, same speed)
    BIT_FORSTNER,       // Forstner bit (0.6x - large face, poor evacuation)
    BIT_SPADE,          // Spade bit (0.7x - aggressive cut)
    BIT_SPADE_SPUR,     // Spade with spurs (0.7x)
    BIT_HOLE_SAW,       // Hole saw (0.4x - very large area)
    BIT_GLASS_TILE,     // Glass/tile bit (0.6x - prevent cracking)
    BIT_AUGER,          // Auger bit (0.8x - deep flutes)
    BIT_STEP,           // Step drill (use step drill mode instead)
    BIT_COUNT
} bit_type_t;

typedef struct {
    const char* name;
    uint8_t factor_x10;  // Speed multiplier * 10 (e.g., 10=1.0x, 6=0.6x)
} bit_type_data_t;

// Bit type speed factors
static const bit_type_data_t bit_types_db[BIT_COUNT] = {
    {"Twist",   10},  // 1.0x baseline
    {"Brad",    10},  // 1.0x (same as twist, just centers)
    {"Forstner", 6},  // 0.6x (large cutting face)
    {"Spade",    7},  // 0.7x (aggressive)
    {"SpdSpur",  7},  // 0.7x (spade with spurs)
    {"HoleSaw",  4},  // 0.4x (very large area)
    {"Glass",    6},  // 0.6x (prevent cracking)
    {"Auger",    8},  // 0.8x (deep flutes)
    {"Step",    10},  // 1.0x (use step drill mode)
};

/*===========================================================================*/
/* Material Database                                                         */
/*===========================================================================*/

typedef struct {
    const char* name;
    uint16_t speed_min;  // Surface speed in m/min (minimum)
    uint16_t speed_max;  // Surface speed in m/min (maximum)
} material_data_t;

// Material surface speed database
static const material_data_t materials_db[MATERIAL_COUNT] = {
    {"Softwood",   45, 55},   // Pine, spruce
    {"Hardwood",   20, 25},   // Oak, maple
    {"Plywood",    35, 45},   // Layered wood
    {"MDF",        40, 50},   // Medium density fiberboard
    {"Aluminum",   75, 90},   // 6061, 7075
    {"Brass",      45, 60},   // Free-cutting brass
    {"Steel",      12, 18},   // Mild steel
    {"Stainless",   8, 12},   // 304, 316 (slower than mild)
    {"Acrylic",    35, 45},   // PMMA, plexiglass
    {"ABS",        30, 40},   // 3D printer plastic
    {"PVC",        40, 50},   // PVC pipe/sheet
    {"Plastic",    35, 45},   // General plastics
};

/*===========================================================================*/
/* Hole saws                                                                 */
/*===========================================================================*/

/* Hole saws are NOT twist drills with a speed factor applied, and treating
 * them as one was badly wrong for metal.
 *
 * bit_types_db gives BIT_HOLE_SAW a 0.4x factor against the twist-drill
 * surface speeds above. Checked against Bosch's own published figures for
 * HSS bi-metal hole saws at 17 mm, that produced:
 *
 *      material      0.4x factor      Bosch
 *      Aluminium     560-673          750
 *      Steel          90-135          500
 *      Stainless      60-90           250
 *
 * - out by four to five times on the ferrous materials. A hole saw cuts a
 * thin kerf at the rim rather than a full-diameter hole, so it tolerates a
 * much higher surface speed than a twist drill in the same material; the
 * relationship is not a constant fraction.
 *
 * Bosch's numbers work out to a constant surface speed per material, which is
 * what this table holds. Verified across their whole diameter range:
 *
 *      Stainless ~13 m/min   (17mm 250 rpm, 51mm 85, 102mm 40)
 *      Steel     ~27 m/min   (17mm 500 rpm, 51mm 170, 102mm 85)
 *      Aluminium ~40 m/min   (17mm 750 rpm, 51mm 255, 102mm 130)
 *
 * Source: Bosch accessories catalogue, "Speed recommendation for HSS Bi-metal
 * and Sheet Metal holesaws", cross-checked against the printed packaging of
 * two current PRO Multi Material saws (2 608 901 492, 17 mm, and
 * 2 608 901 513, 51 mm) whose labels reproduce the table exactly. Hoffmann
 * Group's bi-metal datasheets independently give ~41 m/min for aluminium.
 *
 * HONEST ABOUT PROVENANCE: the metal rows are Bosch's, and match to within a
 * few rpm. Bosch publishes ONE "wood/PVC" column and no brass row, so the
 * wood and plastic entries here are fitted to that single column (which is
 * itself not constant surface speed - it is capped at 3000 rpm for small
 * diameters), and hardwood and brass are scaled down from their siblings in
 * the same proportion the twist-drill table uses. Those are reasoned, not
 * sourced. */
typedef struct {
    uint16_t speed_min;  // Surface speed in m/min, hole saw
    uint16_t speed_max;
} hole_saw_data_t;

static const hole_saw_data_t hole_saw_db[MATERIAL_COUNT] = {
    {280, 360},   // Softwood   - fitted to Bosch's wood/PVC column
    {200, 260},   // Hardwood   - scaled down from softwood
    {250, 330},   // Plywood
    {250, 330},   // MDF
    { 38,  44},   // Aluminum   - Bosch ~40 m/min
    { 24,  30},   // Brass      - scaled from aluminium
    { 25,  29},   // Steel      - Bosch ~27 m/min
    { 12,  14},   // Stainless  - Bosch ~13 m/min
    {200, 280},   // Acrylic
    {200, 280},   // ABS
    {250, 330},   // PVC
    {200, 280},   // Plastic
};

/* Bosch never recommends above 3000 rpm for a hole saw at any diameter, and
 * the small-diameter end of their wood column is flat at exactly that. The
 * low end is the machine's own minimum, not 250: a 102 mm stainless hole saw
 * genuinely wants 40 rpm, and the twist-drill floor of 250 would have
 * recommended six times that. */
#define HOLE_SAW_RPM_CEIL   3000
#define HOLE_SAW_RPM_FLOOR    50

/*===========================================================================*/
/* RPM Calculation                                                           */
/*===========================================================================*/

/**
 * @brief Calculate RPM from surface speed and diameter
 * @param surface_speed_m_min Surface speed in meters/minute
 * @param diameter_mm Drill bit diameter in mm
 * @return Calculated RPM
 *
 * Formula: RPM = (Surface_Speed * 1000) / (π * Diameter)
 */
static inline uint16_t material_calc_rpm_raw(uint16_t surface_speed_m_min, uint16_t diameter_mm) {
    if (diameter_mm == 0) return 500;  // Avoid division by zero

    // RPM = (speed * 1000) / (3.14159 * diameter)
    // Approximate: (speed * 1000) / (diameter * 3.14)
    // Using integer math: (speed * 318) / diameter  (318 ≈ 1000/3.14)
    uint32_t rpm = ((uint32_t)surface_speed_m_min * 318) / diameter_mm;

    if (rpm > 65535) rpm = 65535;
    return (uint16_t)rpm;
}

static inline uint16_t material_calc_rpm(uint16_t surface_speed_m_min, uint16_t diameter_mm) {
    uint32_t rpm = material_calc_rpm_raw(surface_speed_m_min, diameter_mm);

    if (rpm < 250) rpm = 250;
    if (rpm > 5500) rpm = 5500;

    return (uint16_t)rpm;
}

/**
 * @brief Get material name string
 */
static inline const char* material_get_name(material_type_t mat) {
    if (mat >= MATERIAL_COUNT) return "Unknown";
    return materials_db[mat].name;
}

/**
 * @brief Calculate RPM range for material, bit type, and diameter
 * @param mat Material type
 * @param bit_type Bit type (for speed factor)
 * @param diameter_mm Bit diameter in mm
 * @param rpm_min Output: minimum RPM
 * @param rpm_max Output: maximum RPM
 */
static inline void material_calc_rpm_range(material_type_t mat, bit_type_t bit_type,
                                          uint16_t diameter_mm,
                                          uint16_t* rpm_min, uint16_t* rpm_max) {
    if (mat >= MATERIAL_COUNT) {
        *rpm_min = 500;
        *rpm_max = 1500;
        return;
    }

    if (bit_type >= BIT_COUNT) bit_type = BIT_TWIST;

    /* The machine's usable spindle range. Both ends of the recommended band
     * are clamped into it — see the note at the bottom of this function. */
    #define MATERIAL_RPM_FLOOR  250
    #define MATERIAL_RPM_CEIL   5500

    uint16_t lo_clamp = MATERIAL_RPM_FLOOR;
    uint16_t hi_clamp = MATERIAL_RPM_CEIL;

    if (bit_type == BIT_HOLE_SAW) {
        /* Own surface speeds, own limits — see hole_saw_db above. No
         * factor_x10 is applied: that factor is what was wrong. */
        *rpm_min = material_calc_rpm_raw(hole_saw_db[mat].speed_min, diameter_mm);
        *rpm_max = material_calc_rpm_raw(hole_saw_db[mat].speed_max, diameter_mm);
        lo_clamp = HOLE_SAW_RPM_FLOOR;
        hi_clamp = HOLE_SAW_RPM_CEIL;
    } else {

    // Calculate base RPM from material
    uint16_t base_min = material_calc_rpm(materials_db[mat].speed_min, diameter_mm);
    uint16_t base_max = material_calc_rpm(materials_db[mat].speed_max, diameter_mm);

    // Apply bit type factor (factor_x10 / 10)
    uint8_t factor = bit_types_db[bit_type].factor_x10;
    *rpm_min = (base_min * factor) / 10;
    *rpm_max = (base_max * factor) / 10;
    }

    /* AUDIT FIX (MEDIUM, materials.h:157): min was only floor-clamped and max
     * only ceiling-clamped, so a combination whose whole band sits outside the
     * machine's range came back INVERTED. Stainless + hole saw + 50 mm yields
     * min 250 / max 100 — and the caller's midpoint of an inverted band is a
     * number that was never in the band at all, which then reaches
     * g_state.target_rpm. Clamp both ends to the same range, then restore the
     * ordering invariant that callers rely on. */
    if (*rpm_min < lo_clamp) *rpm_min = lo_clamp;
    if (*rpm_min > hi_clamp) *rpm_min = hi_clamp;
    if (*rpm_max < lo_clamp) *rpm_max = lo_clamp;
    if (*rpm_max > hi_clamp) *rpm_max = hi_clamp;
    if (*rpm_max < *rpm_min) *rpm_max = *rpm_min;
}

#endif /* MATERIALS_H */
