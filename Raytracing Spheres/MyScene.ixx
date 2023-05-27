module;

#include "directxtk12/GamePad.h"
#include "directxtk12/Keyboard.h"
#include "directxtk12/Mouse.h"

#include "directxtk12/SimpleMath.h"

#include "directxtk12/GeometricPrimitive.h"

#include "PhysX.h"

#include <ranges>

#include <filesystem>

export module MyScene;

import Material;
import Random;
import Scene;
import Texture;

using namespace DirectX;
using namespace DirectX::SimpleMath;
using namespace physx;
using namespace PhysicsHelpers;
using namespace std;
using namespace std::filesystem;

using GamepadButtonState = GamePad::ButtonStateTracker::ButtonState;
using Key = Keyboard::Keys;

#define MAKE_NAME(name) static constexpr const char* name = #name;

namespace {
	struct ObjectNames {
		MAKE_NAME(AlienMetal);
		MAKE_NAME(Earth);
		MAKE_NAME(HarmonicOscillator);
		MAKE_NAME(Moon);
		MAKE_NAME(Sphere);
		MAKE_NAME(Star);
	};

	constexpr struct { PxReal PositionY = 0.5f, Period = 3; } Spring;
}

export {
	struct MySceneDesc : SceneDesc {
		MySceneDesc() {
			{
				GeometricPrimitive::VertexCollection vertices;
				GeometricPrimitive::IndexCollection indices;
				GeometricPrimitive::CreateGeoSphere(vertices, indices, 1, 6);
				Meshes[ObjectNames::Sphere] = { vertices, indices };
			}

			Camera.Position.z = -15;

			const path directoryPath = LR"(Assets\Textures)";

			EnvironmentLightCubeMap = { directoryPath / L"Space.dds", Matrix::CreateFromYawPitchRoll(XM_PI * 0.2f, XM_PI, 0) };

			const auto& material = *PhysX->GetPhysics().createMaterial(0.5f, 0.5f, 0.6f);

			const auto AddRenderObject = [&](RenderObjectDesc& renderObject, const auto& transform, const PxSphereGeometry& geometry) -> decltype(auto) {
				renderObject.MeshURI = ObjectNames::Sphere;

				auto& rigidDynamic = *PhysX->GetPhysics().createRigidDynamic(PxTransform(transform));
				PxRigidBodyExt::updateMassAndInertia(rigidDynamic, 1);
				rigidDynamic.setAngularDamping(0);
				PhysX->GetScene().addActor(rigidDynamic);

				renderObject.Shape = PxRigidActorExt::createExclusiveShape(rigidDynamic, geometry, material);

				RenderObjects.emplace_back(renderObject);

				return rigidDynamic;
			};

			{
				const struct {
					const char* Name;
					PxVec3 Position;
					Material Material;
				} objects[]{
					{
						.Name = ObjectNames::AlienMetal,
						.Position{ -2, 0.5f, 0 },
						.Material{
							.BaseColor{ 0.1f, 0.2f, 0.5f, 1 },
							.Roughness = 0.9f
						}
					},
					{
						.Position{ 0, 0.5f, 0 },
						.Material{
							.BaseColor{ 1, 1, 1, 1 },
							.Roughness = 0,
							.Opacity = 0,
							.RefractiveIndex = 1.5f,
						}
					},
					{
						.Position{ 0, 2, 0 },
						.Material{
							.BaseColor{ 1, 1, 1, 1 },
							.Roughness = 0.5f,
							.Opacity = 0,
							.RefractiveIndex = 1.5f,
						}
					},
					{
						.Position{ 2, 0.5f, 0 },
						.Material{
							.BaseColor{ 0.7f, 0.6f, 0.5f, 1 },
							.Metallic = 1,
							.Roughness = 0.3f
						}
					}
				};
				for (const auto& [Name, Position, Material] : objects) {
					RenderObjectDesc renderObject;

					renderObject.Material = Material;

					if (auto& textures = renderObject.Textures; Name == ObjectNames::AlienMetal) {
						textures[TextureType::BaseColorMap] = directoryPath / L"Alien-Metal_Albedo.png";
						textures[TextureType::NormalMap] = directoryPath / L"Alien-Metal_Normal.png";
						textures[TextureType::MetallicMap] = directoryPath / L"Alien-Metal_Metallic.png";
						textures[TextureType::RoughnessMap] = directoryPath / L"Alien-Metal_Roughness.png";
						textures[TextureType::AmbientOcclusionMap] = directoryPath / L"Alien-Metal_AO.png";
					}

					AddRenderObject(renderObject, Position, PxSphereGeometry(0.5f));
				}

				for (Random random; const auto i : views::iota(-10, 11)) {
					for (const auto j : views::iota(-10, 11)) {
						constexpr auto A = 0.5f;
						const auto omega = PxTwoPi / Spring.Period;

						PxVec3 position;
						position.x = static_cast<float>(i) + 0.7f * random.Float();
						position.y = Spring.PositionY + SimpleHarmonicMotion::Spring::CalculateDisplacement(A, omega, 0.0f, position.x);
						position.z = static_cast<float>(j) - 0.7f * random.Float();

						bool isOverlapping = false;
						for (const auto& [_, Position, Material] : objects) {
							if ((position - Position).magnitude() < 1) {
								isOverlapping = true;
								break;
							}
						}
						if (isOverlapping) continue;

						RenderObjectDesc renderObject;

						renderObject.Name = ObjectNames::HarmonicOscillator;

						constexpr auto RandomVector4 = [&](float min) {
							const auto value = random.Float3();
							return Vector4(value.x, value.y, value.z, 1);
						};
						if (const auto randomValue = random.Float();
							randomValue < 0.3f) {
							renderObject.Material = { .BaseColor = RandomVector4(0.1f) };
						}
						else if (randomValue < 0.6f) {
							renderObject.Material = {
								.BaseColor = RandomVector4(0.1f),
								.Metallic = 1,
								.Roughness = random.Float(0, 0.5f)
							};
						}
						else if (randomValue < 0.8f) {
							renderObject.Material = {
								.BaseColor = RandomVector4(0.1f),
								.Roughness = random.Float(0, 0.5f),
								.Opacity = 0,
								.RefractiveIndex = 1.5f
							};
						}
						else {
							renderObject.Material = {
								.BaseColor = RandomVector4(0.1f),
								.EmissiveColor = RandomVector4(0.2f),
								.Metallic = random.Float(0.4f),
								.Roughness = random.Float(0.3f)
							};
						}

						AddRenderObject(renderObject, position, PxSphereGeometry(0.075f)).setLinearVelocity({ 0, SimpleHarmonicMotion::Spring::CalculateVelocity(A, omega, 0.0f, position.x), 0 });
					}
				}
			}

			{
				const struct {
					LPCSTR Name;
					PxVec3 Position;
					PxReal Radius, RotationPeriod, OrbitalPeriod, Mass;
					Material Material;
				} moon{
					.Name = ObjectNames::Moon,
					.Position{ -4, 4, 0 },
					.Radius = 0.25f,
					.OrbitalPeriod = 10,
					.Material{
						.BaseColor{ 0.5f, 0.5f, 0.5f, 1 },
						.Roughness = 0.8f
					}
				}, earth{
					.Name = ObjectNames::Earth,
					.Position{ 0, moon.Position.y, 0 },
					.Radius = 1,
					.RotationPeriod = 15,
					.Mass = UniversalGravitation::CalculateMass((moon.Position - earth.Position).magnitude(), moon.OrbitalPeriod),
					.Material{
						.BaseColor{ 0.3f, 0.4f, 0.5f, 1 },
						.Roughness = 0.8f
					}
				}, star{
					.Name = ObjectNames::Star,
					.Position{ 0, -50.1f, 0 },
					.Radius = 50,
					.Material{
						.BaseColor{ 0.5f, 0.5f, 0.5f, 1 },
						.Metallic = 1,
						.Roughness = 0
					}
				};
				for (const auto& [Name, Position, Radius, RotationPeriod, OrbitalPeriod, Mass, Material] : { moon, earth, star }) {
					RenderObjectDesc renderObject;

					renderObject.Name = Name;

					renderObject.Material = Material;

					auto& rigidDynamic = AddRenderObject(renderObject, Position, PxSphereGeometry(Radius));

					if (auto& textures = RenderObjects.back().Textures;
						renderObject.Name == ObjectNames::Moon) {
						const auto x = earth.Position - Position;
						const auto magnitude = x.magnitude();
						const auto normalized = x / magnitude;
						const auto linearSpeed = UniversalGravitation::CalculateFirstCosmicSpeed(earth.Mass, magnitude);
						rigidDynamic.setLinearVelocity(linearSpeed * PxVec3(-normalized.z, 0, normalized.x));
						rigidDynamic.setAngularVelocity({ 0, linearSpeed / magnitude, 0 });

						textures[TextureType::BaseColorMap] = directoryPath / L"Moon_BaseColor.jpg";
						textures[TextureType::NormalMap] = directoryPath / L"Moon_Normal.jpg";
						RenderObjects.back().TextureTransform = Matrix::CreateTranslation(0.5f, 0, 0);
					}
					else if (renderObject.Name == ObjectNames::Earth) {
						rigidDynamic.setAngularVelocity({ 0, PxTwoPi / RotationPeriod, 0 });
						PxRigidBodyExt::setMassAndUpdateInertia(rigidDynamic, &Mass, 1);

						textures[TextureType::BaseColorMap] = directoryPath / L"Earth_BaseColor.jpg";
						textures[TextureType::NormalMap] = directoryPath / L"Earth_Normal.jpg";
					}
					else if (renderObject.Name == ObjectNames::Star) rigidDynamic.setMass(0);

					RigidBodies[Name] = &rigidDynamic;
				}
			}
		}
	};

	struct MyScene : Scene {
		bool IsWorldStatic() const override { return !m_isSimulatingPhysics; }

		void Tick(double elapsedSeconds, const GamePad::ButtonStateTracker& gamepadStateTracker, const Keyboard::KeyboardStateTracker& keyboardStateTracker, const Mouse::ButtonStateTracker& mouseStateTracker) override {
			if (mouseStateTracker.GetLastState().positionMode == Mouse::MODE_RELATIVE) {
				if (gamepadStateTracker.a == GamepadButtonState::PRESSED) m_isSimulatingPhysics = !m_isSimulatingPhysics;
				if (keyboardStateTracker.IsKeyPressed(Key::Space)) m_isSimulatingPhysics = !m_isSimulatingPhysics;

				{
					auto& isGravityEnabled = reinterpret_cast<bool&>(RigidBodies.at(ObjectNames::Earth)->userData);
					if (gamepadStateTracker.b == GamepadButtonState::PRESSED) isGravityEnabled = !isGravityEnabled;
					if (keyboardStateTracker.IsKeyPressed(Key::G)) isGravityEnabled = !isGravityEnabled;
				}

				{
					auto& isGravityEnabled = reinterpret_cast<bool&>(RigidBodies.at(ObjectNames::Star)->userData);
					if (gamepadStateTracker.y == GamepadButtonState::PRESSED) isGravityEnabled = !isGravityEnabled;
					if (keyboardStateTracker.IsKeyPressed(Key::H)) isGravityEnabled = !isGravityEnabled;
				}
			}

			if (!m_isSimulatingPhysics) return;

			for (auto& renderObject : RenderObjects) {
				const auto& shape = *renderObject.Shape;

				const auto rigidBody = shape.getActor()->is<PxRigidBody>();
				if (rigidBody == nullptr) continue;

				const auto mass = rigidBody->getMass();
				if (!mass) continue;

				const auto& position = PxShapeExt::getGlobalPose(shape, *shape.getActor()).p;

				if (const auto& [PositionY, Period] = Spring;
					renderObject.Name == ObjectNames::HarmonicOscillator) {
					const auto k = SimpleHarmonicMotion::Spring::CalculateConstant(mass, Period);
					const PxVec3 x(0, position.y - PositionY, 0);
					rigidBody->addForce(-k * x);
				}

				if (const auto& earth = *RigidBodies.at(ObjectNames::Earth);
					(reinterpret_cast<const bool&>(earth.userData) && renderObject.Name != ObjectNames::Earth)
					|| renderObject.Name == ObjectNames::Moon) {
					const auto x = earth.getGlobalPose().p - position;
					const auto magnitude = x.magnitude();
					const auto normalized = x / magnitude;
					rigidBody->addForce(UniversalGravitation::CalculateAccelerationMagnitude(earth.getMass(), magnitude) * normalized, PxForceMode::eACCELERATION);
				}

				if (const auto& star = *RigidBodies.at(ObjectNames::Star);
					reinterpret_cast<const bool&>(star.userData) && renderObject.Name != ObjectNames::Star) {
					const auto x = star.getGlobalPose().p - position;
					const auto normalized = x.getNormalized();
					rigidBody->addForce(10 * normalized, PxForceMode::eACCELERATION);
				}
			}

			PhysX->Tick(static_cast<PxReal>(min(1.0 / 60, elapsedSeconds)));
		}

	private:
		bool m_isSimulatingPhysics = true;
	};
}
