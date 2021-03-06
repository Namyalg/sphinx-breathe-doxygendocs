#include "moja/modules/cbm/peatlandturnovermodule.h"
#include "moja/modules/cbm/printpools.h"
#include "moja/modules/cbm/timeseries.h"

#include <moja/flint/ivariable.h>
#include <moja/flint/ipool.h>
#include <moja/flint/ioperation.h>
#include <moja/signals.h>
#include <moja/notificationcenter.h>
#include <moja/logging.h>

namespace moja {
	namespace modules {
		namespace cbm {

			void PeatlandTurnoverModule::doLocalDomainInit() {
				_atmosphere = _landUnitData->getPool("Atmosphere");

				_woodyFoliageLive = _landUnitData->getPool("WoodyFoliageLive");
				_woodyStemsBranchesLive = _landUnitData->getPool("WoodyStemsBranchesLive");
				_woodyRootsLive = _landUnitData->getPool("WoodyRootsLive");

				_sedgeFoliageLive = _landUnitData->getPool("SedgeFoliageLive");
				_sedgeRootsLive = _landUnitData->getPool("SedgeRootsLive");

				_sphagnumMossLive = _landUnitData->getPool("SphagnumMossLive");
				_featherMossLive = _landUnitData->getPool("FeatherMossLive");

				_woodyFoliageDead = _landUnitData->getPool("WoodyFoliageDead");
				_woodyFineDead = _landUnitData->getPool("WoodyFineDead");
				_woodyCoarseDead = _landUnitData->getPool("WoodyCoarseDead");
				_woodyRootsDead = _landUnitData->getPool("WoodyRootsDead");

				_sedgeFoliageDead = _landUnitData->getPool("SedgeFoliageDead");
				_sedgeRootsDead = _landUnitData->getPool("SedgeRootsDead");

				_feathermossDead = _landUnitData->getPool("FeathermossDead");

				_acrotelm_o = _landUnitData->getPool("Acrotelm_O");
				_catotelm_a = _landUnitData->getPool("Catotelm_A");
				_acrotelm_a = _landUnitData->getPool("Acrotelm_A");
				_catotelm_o = _landUnitData->getPool("Catotelm_O");

				_spinupMossOnly = _landUnitData->getVariable("spinup_moss_only");
				_regenDelay = _landUnitData->getVariable("regen_delay");

				baseWTDParameters = _landUnitData->getVariable("base_wtd_parameters")->value().extract<DynamicObject>();
				_waterTableDepthModifier = _landUnitData->getVariable("peatland_annual_wtd_modifiers");
			}

			void PeatlandTurnoverModule::doTimingInit() {
				_runPeatland = false;
				_forward_wtd_modifier = "";
				_modifiersFullyAppplied = false;

				auto& peatland_class = _landUnitData->getVariable("peatland_class")->value();
				_peatlandId = peatland_class.isEmpty() ? -1 : peatland_class.convert<int>();

				if (_peatlandId > 0) {
					_runPeatland = true;

					// get the data by variable "peatland_turnover_parameters"
					const auto& peatlandTurnoverParams = _landUnitData->getVariable("peatland_turnover_parameters")->value();

					//create the PeaglandGrowthParameters, set the value from the variable
					turnoverParas = std::make_shared<PeatlandTurnoverParameters>();
					turnoverParas->setValue(peatlandTurnoverParams.extract<DynamicObject>());

					// get the data by variable "peatland_growth_parameters"
					const auto& peatlandGrowthParams = _landUnitData->getVariable("peatland_growth_parameters")->value();

					//create the PeatlandGrowthParameters, set the value from the variable
					growthParas = std::make_shared<PeatlandGrowthParameters>();
					if (!peatlandGrowthParams.isEmpty()) {
						growthParas->setValue(peatlandGrowthParams.extract<DynamicObject>());
					}

					auto& lnMDroughtCode = _landUnitData->getVariable("forward_drought_class")->value();
					auto& defaultLMDC = _landUnitData->getVariable("default_forward_drought_class")->value();
					auto lnMeanDroughtCode = lnMDroughtCode.isEmpty() ? defaultLMDC : lnMDroughtCode;
					auto lwtd = computeWaterTableDepth(lnMeanDroughtCode, _peatlandId);

					//set the long term water table depth variable value as initial status		
					_forward_longterm_wtd = lwtd;
					_forward_previous_annual_wtd = lwtd;
					_forward_current_annual_wtd = lwtd;
				}
			}

			void PeatlandTurnoverModule::doTimingStep() {
				bool spinupMossOnly = _spinupMossOnly->value();
				if (spinupMossOnly) { return; }

				if (_runPeatland) {
					updateWaterTable();

					int regenDelay = _regenDelay->value();
					if (regenDelay > 0) {
						//in delay period, no any growth
						//do flux between catotelm and acrotelm due to water table changes
						doWaterTableFlux();
					}
					else {
						//update the current pool value
						updatePeatlandLivePoolValue();

						//turnover on live pools
						doLivePoolTurnover();

						//flux between catotelm and acrotelm due to water table changes
						doWaterTableFlux();
					}
				}
			}

			double PeatlandTurnoverModule::getModifiedAnnualWTD(std::string modifierStr) {
				//set default current yeare WTD as forward long term WTD
				double newCurrentYearWtd = _forward_longterm_wtd;

				//new concept, after the disturbnace, apply the modifier(WTD) up to years
				std::string yearStr = modifierStr.substr(0, modifierStr.find_first_of("_"));
				int years = std::stoi(yearStr);

				std::string currentModiferStr = modifierStr.substr(modifierStr.find_first_of("_") + 1);
				int modifierValue = std::stoi(currentModiferStr);

				//default WTD modifier are "-1,0" or "0,0", years=0/-1, modifierValue=0
				//apply and update only if years > 0
				if (years > 0) {
					years -= 1;

					//use the modifier WTD to replace the current WTD, new concept
					newCurrentYearWtd = modifierValue;

					if (years == 0) {
						// years to modifier reached, no more modification further
						_forward_wtd_modifier = "";
						_modifiersFullyAppplied = true;
					}
					else {
						//update the forward modifier for remaining years
						_forward_wtd_modifier = std::to_string(years) + "_" + std::to_string(modifierValue);;
					}
				}
				return newCurrentYearWtd;
			}

			void PeatlandTurnoverModule::updateWaterTable() {
				//get the default annual drought code
				auto& defaultAnnualDC = _landUnitData->getVariable("default_annual_drought_class")->value();

				//get the current annual drought code
				auto& annualDC = _landUnitData->getVariable("annual_drought_class")->value();
				double annualDroughtCode = annualDC.isEmpty() ? defaultAnnualDC.convert<double>()
					: annualDC.type() == typeid(TimeSeries) ? annualDC.extract<TimeSeries>().value()
					: annualDC.convert<double>();

				//compute the water table depth parameter to be used in current step
				double newCurrentYearWtd = computeWaterTableDepth(annualDroughtCode, _peatlandId);

				//try to apply the WTD modifier and update the current year WTD
				if (!_modifiersFullyAppplied) {//modifier is neither applied nor used up
					//if the local modifier is empty, it means it is never set before
					if (_forward_wtd_modifier.empty()) {
						//then check if there is a valid WTD modifer trigged in event
						std::string waterTableModifier = _waterTableDepthModifier->value();
						if (!waterTableModifier.empty()) {
							//there is a WTD modifier, get the valid WTD value for current step
							//and updated the modifiers accordingly (remainging years to apply)
							newCurrentYearWtd = getModifiedAnnualWTD(waterTableModifier);
						}
					}
					else {
						//forward WTD modifier is already set, get the valid WTD value 
						//and update remaining modifiers accordingly
						newCurrentYearWtd = getModifiedAnnualWTD(_forward_wtd_modifier);
					}
				}

				//set the previous annual WTD with the not updated current WTD value
				_forward_previous_annual_wtd = _forward_current_annual_wtd;

				//update the current WTD with newly computed WTD value	
				_forward_current_annual_wtd = newCurrentYearWtd;
			}

			void PeatlandTurnoverModule::doWaterTableFlux() {
				//get current annual water table depth
				double currentAwtd = _forward_current_annual_wtd;

				//get previous annual water table depth
				double previousAwtd = _forward_previous_annual_wtd;

				//get long term annual water table depth
				double longtermWtd = _forward_longterm_wtd;

				currentAwtd = currentAwtd > 0.0 ? 0.0 : currentAwtd;
				previousAwtd = previousAwtd > 0.0 ? 0.0 : previousAwtd;
				longtermWtd = longtermWtd > 0.0 ? 0.0 : longtermWtd;

				double a = turnoverParas->a();
				double b = turnoverParas->b();

				auto peatlandWaterTableFlux = _landUnitData->createStockOperation();

				double coPoolValue = _catotelm_o->value();
				double caPoolValue = _catotelm_a->value();
				double aoPoolValue = _acrotelm_o->value();
				double aaPoolValue = _acrotelm_a->value();

				double fluxAmount = computeCarbonTransfers(previousAwtd, currentAwtd, a, b);

				if (currentAwtd < longtermWtd && previousAwtd < longtermWtd) {
					if (currentAwtd >= previousAwtd) {
						//Catotelm_O -> Catotelm_A 		
						if (fluxAmount > coPoolValue) fluxAmount = coPoolValue;
						peatlandWaterTableFlux->addTransfer(_catotelm_o, _catotelm_a, fluxAmount);
					}
					else if (currentAwtd <= previousAwtd) {
						//Catotelm_A -> Catotelm_O
						if (fluxAmount > caPoolValue) fluxAmount = caPoolValue;
						peatlandWaterTableFlux->addTransfer(_catotelm_a, _catotelm_o, fluxAmount);
					}
				}
				else if (currentAwtd > longtermWtd&& previousAwtd > longtermWtd) {
					if (currentAwtd >= previousAwtd) {
						//Acrotelm_O -> Acrotelm_A 				
						if (fluxAmount > aoPoolValue) fluxAmount = aoPoolValue;
						peatlandWaterTableFlux->addTransfer(_acrotelm_o, _acrotelm_a, fluxAmount);
					}
					else if (currentAwtd <= previousAwtd) {
						//Acrotelm_A -> Acrotelm_O 
						if (fluxAmount > aaPoolValue) fluxAmount = aaPoolValue;
						peatlandWaterTableFlux->addTransfer(_acrotelm_a, _acrotelm_o, fluxAmount);
					}
				}
				else if (currentAwtd >= longtermWtd && previousAwtd <= longtermWtd) {
					if (currentAwtd >= previousAwtd) {
						double ao2aa = computeCarbonTransfers(longtermWtd, currentAwtd, a, b);
						if (ao2aa > aoPoolValue) ao2aa = aoPoolValue;
						peatlandWaterTableFlux->addTransfer(_acrotelm_o, _acrotelm_a, ao2aa);

						double co2ca = computeCarbonTransfers(longtermWtd, previousAwtd, a, b);
						if (co2ca > coPoolValue) co2ca = coPoolValue;
						peatlandWaterTableFlux->addTransfer(_catotelm_o, _catotelm_a, co2ca);
					}
				}
				else if (currentAwtd <= longtermWtd && previousAwtd >= longtermWtd) {
					if (currentAwtd <= previousAwtd) {
						double aa2ao = computeCarbonTransfers(longtermWtd, previousAwtd, a, b);
						if (aa2ao > aaPoolValue) aa2ao = aaPoolValue;
						peatlandWaterTableFlux->addTransfer(_acrotelm_a, _acrotelm_o, aa2ao);

						double ca2co = computeCarbonTransfers(longtermWtd, currentAwtd, a, b);
						if (ca2co > caPoolValue) ca2co = caPoolValue;
						peatlandWaterTableFlux->addTransfer(_catotelm_a, _catotelm_o, ca2co);
					}
				}
				_landUnitData->submitOperation(peatlandWaterTableFlux);
				_landUnitData->applyOperations();
			}
		}
	}
} // namespace moja::modules::cbm
