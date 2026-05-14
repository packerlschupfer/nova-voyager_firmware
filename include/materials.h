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
static inline uint16_t material_calc_rpm(uint16_t surface_speed_m_min, uint16_t diameter_mm) {
    if (diameter_mm == 0) return 500;  // Avoid division by zero

    // RPM = (speed * 1000) / (3.14159 * diameter)
    // Approximate: (speed * 1000) / (diameter * 3.14)
    // Using integer math: (speed * 318) / diameter  (318 ≈ 1000/3.14)
    uint32_t rpm = ((uint32_t)surface_speed_m_min * 318) / diameter_mm;

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

    // Calculate base RPM from material
    uint16_t base_min = material_calc_rpm(materials_db[mat].speed_min, diameter_mm);
    uint16_t base_max = material_calc_rpm(materials_db[mat].speed_max, diameter_mm);

    // Apply bit type factor (factor_x10 / 10)
    uint8_t factor = bit_types_db[bit_type].factor_x10;
    *rpm_min = (base_min * factor) / 10;
    *rpm_max = (base_max * factor) / 10;

    // Ensure within valid range
    if (*rpm_min < 250) *rpm_min = 250;
    if (*rpm_max > 5500) *rpm_max = 5500;
}

#endif /* MATERIALS_H */
