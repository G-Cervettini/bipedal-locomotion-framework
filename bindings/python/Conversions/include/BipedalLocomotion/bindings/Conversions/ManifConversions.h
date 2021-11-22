/**
 * @file ManifConversions.h
 * @authors Giulio Romualdi
 * @copyright 2021 Istituto Italiano di Tecnologia (IIT). This software may be modified and
 * distributed under the terms of the GNU Lesser General Public License v2.1 or any later version.
 */

#ifndef BIPEDAL_LOCOMOTION_BINDINGS_CONVERSIONS_MANIF_CONVERSIONS_H
#define BIPEDAL_LOCOMOTION_BINDINGS_CONVERSIONS_MANIF_CONVERSIONS_H

#include <pybind11/pybind11.h>

namespace BipedalLocomotion
{
namespace bindings
{
namespace Conversions
{

void CreateManifConversions(pybind11::module& module);

} // namespace Conversions
} // namespace bindings
} // namespace BipedalLocomotion

#endif // BIPEDAL_LOCOMOTION_BINDINGS_CONVERSIONS_MANIF_CONVERSIONS_H
