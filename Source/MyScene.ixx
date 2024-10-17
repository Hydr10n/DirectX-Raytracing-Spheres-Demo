module;

#include <filesystem>
#include <ranges>

#include "directxtk12/GamePad.h"
#include "directxtk12/GeometricPrimitive.h"
#include "directxtk12/Keyboard.h"
#include "directxtk12/Mouse.h"
#include "directxtk12/SimpleMath.h"

#include "PhysX.h"

export module MyScene;

export import Scene;

import Material;
import Model;
import Random;
import Texture;

using namespace DirectX;
using namespace DirectX::SimpleMath;
using namespace PhysicsHelpers;
using namespace physx;
using namespace std;
using namespace std::filesystem;

using GamepadButtonState = GamePad::ButtonStateTracker::ButtonState;
using Key = Keyboard::Keys;

#define MAKE_NAME(name) static constexpr LPCSTR name = #name;

struct ObjectNames {
	MAKE_NAME(AlienMetal);
	MAKE_NAME(Earth);
	MAKE_NAME(HarmonicOscillator);
	MAKE_NAME(Moon);
	MAKE_NAME(Sphere);
	MAKE_NAME(Star);
};

struct Spring { static constexpr PxReal PositionY = 0.5f, Period = 3; };

export {
	struct MySceneDesc : SceneDesc {
		MySceneDesc() {
			{
				GeometricPrimitive::VertexCollection vertices;
				GeometricPrimitive::IndexCollection indices;
				GeometricPrimitive::CreateGeoSphere(vertices, indices, 1, 6);
				Meshes[ObjectNames::Sphere] = { make_shared<vector<VertexPositionNormalTexture>>(vertices), make_shared<vector<Mesh::IndexType>>(indices) };
			}

			Camera.Position.z = -15;

			const path directoryPath = L"Assets/Textures";

			EnvironmentLightTexture = {
				.FilePath = directoryPath / L"141_hdrmaps_com_free.exr",
				.Transform{
					.Rotation = Quaternion::CreateFromYawPitchRoll(XM_PI, 0, 0)
				}
			};

			PhysX = make_shared<::PhysX>(8);

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
					LPCSTR Name;
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
							.Transmission = 1,
							.IOR = 1.5f
						}
					},
					{
						.Position{ 0, 2, 0 },
						.Material{
							.BaseColor{ 1, 1, 1, 1 },
							.Roughness = 0.5f,
							.Transmission = 1,
							.IOR = 1.5f
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
						textures[to_underlying(TextureMapType::BaseColor)] = directoryPath / L"Alien-Metal_Albedo.png";
						textures[to_underlying(TextureMapType::Metallic)] = directoryPath / L"Alien-Metal_Metallic.png";
						textures[to_underlying(TextureMapType::Roughness)] = directoryPath / L"Alien-Metal_Roughness.png";
						textures[to_underlying(TextureMapType::Normal)] = directoryPath / L"Alien-Metal_Normal.png";
					}

					AddRenderObject(renderObject, Position, PxSphereGeometry(0.5f));
				}

				for (Random random; const auto i : views::iota(-10, 11)) {
					for (const auto j : views::iota(-10, 11)) {
						constexpr auto A = 0.5f;
						const auto omega = PxTwoPi / Spring::Period;

						PxVec3 position;
						position.x = static_cast<float>(i) + 0.7f * random.Float();
						position.y = Spring::PositionY + SimpleHarmonicMotion::Spring::CalculateDisplacement(A, omega, 0.0f, position.x);
						position.z = static_cast<float>(j) - 0.7f * random.Float();

						auto isOverlapping = false;
						for (const auto& [_, Position, Material] : objects) {
							if ((position - Position).magnitude() < 1) {
								isOverlapping = true;
								break;
							}
						}
						if (isOverlapping) continue;

						RenderObjectDesc renderObject;

						renderObject.Name = ObjectNames::HarmonicOscillator;

						const auto RandomFloat4 = [&](float min) {
							const auto value = random.Float3(min);
							return Vector4(value.x, value.y, value.z, 1);
						};
						if (const auto randomValue = random.Float();
							randomValue < 0.3f) {
							renderObject.Material = { .BaseColor = RandomFloat4(0.1f) };
						}
						else if (randomValue < 0.6f) {
							renderObject.Material = {
								.BaseColor = RandomFloat4(0.1f),
								.Metallic = 1,
								.Roughness = random.Float(0, 0.5f)
							};
						}
						else if (randomValue < 0.8f) {
							renderObject.Material = {
								.BaseColor = RandomFloat4(0.1f),
								.Roughness = random.Float(0, 0.5f),
								.Transmission = 1,
								.IOR = 1.5f
							};
						}
						else {
							renderObject.Material = {
								.BaseColor = RandomFloat4(0.1f),
								.EmissiveColor = random.Float3(0.2f),
								.EmissiveIntensity = random.Float(1, 10),
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

						textures[to_underlying(TextureMapType::BaseColor)] = directoryPath / L"Moon_BaseColor.jpg";
						textures[to_underlying(TextureMapType::Normal)] = directoryPath / L"Moon_Normal.jpg";
					}
					else if (renderObject.Name == ObjectNames::Earth) {
						rigidDynamic.setAngularVelocity({ 0, PxTwoPi / RotationPeriod, 0 });
						PxRigidBodyExt::setMassAndUpdateInertia(rigidDynamic, &Mass, 1);

						textures[to_underlying(TextureMapType::BaseColor)] = directoryPath / L"Earth_BaseColor.jpg";
						textures[to_underlying(TextureMapType::Normal)] = directoryPath / L"Earth_Normal.jpg";
					}
					else if (renderObject.Name == ObjectNames::Star) rigidDynamic.setMass(0);

					RigidActors[Name] = &rigidDynamic;
				}
			}
		}
};

struct MyScene : Scene {
	using Scene::Scene;

	bool IsStatic() const override { return !m_isPhysXRunning; }

	void Tick(double elapsedSeconds, const GamePad::ButtonStateTracker& gamepadStateTracker, const Keyboard::KeyboardStateTracker& keyboardStateTracker, const Mouse::ButtonStateTracker& mouseStateTracker) override {
		if (mouseStateTracker.GetLastState().positionMode == Mouse::MODE_RELATIVE) {
			if (gamepadStateTracker.a == GamepadButtonState::PRESSED) m_isPhysXRunning = !m_isPhysXRunning;
			if (keyboardStateTracker.IsKeyPressed(Key::Space)) m_isPhysXRunning = !m_isPhysXRunning;

			{
				auto& isGravityEnabled = reinterpret_cast<bool&>(RigidActors.at(ObjectNames::Earth)->userData);
				if (gamepadStateTracker.b == GamepadButtonState::PRESSED) isGravityEnabled = !isGravityEnabled;
				if (keyboardStateTracker.IsKeyPressed(Key::G)) isGravityEnabled = !isGravityEnabled;
			}

			{
				auto& isGravityEnabled = reinterpret_cast<bool&>(RigidActors.at(ObjectNames::Star)->userData);
				if (gamepadStateTracker.y == GamepadButtonState::PRESSED) isGravityEnabled = !isGravityEnabled;
				if (keyboardStateTracker.IsKeyPressed(Key::H)) isGravityEnabled = !isGravityEnabled;
			}
		}

		if (IsStatic()) return;

		Tick(elapsedSeconds);

		Refresh();
	}

protected:
	void Tick(double elapsedSeconds) override {
		for (auto& renderObject : RenderObjects) {
			const auto& shape = *renderObject.Shape;

			const auto rigidBody = shape.getActor()->is<PxRigidBody>();
			if (rigidBody == nullptr) continue;

			rigidBody->setActorFlag(PxActorFlag::eDISABLE_SIMULATION, !renderObject.IsVisible);
			if (!renderObject.IsVisible) continue;

			const auto mass = rigidBody->getMass();
			if (!mass) continue;

			const auto& position = PxShapeExt::getGlobalPose(shape, *shape.getActor()).p;

			if (renderObject.Name == ObjectNames::HarmonicOscillator) {
				const auto k = SimpleHarmonicMotion::Spring::CalculateConstant(mass, Spring::Period);
				const PxVec3 x(0, position.y - Spring::PositionY, 0);
				rigidBody->addForce(-k * x);
			}

			if (const auto& earth = *RigidActors.at(ObjectNames::Earth)->is<PxRigidDynamic>();
				(static_cast<bool>(earth.userData) && renderObject.Name != ObjectNames::Earth)
				|| renderObject.Name == ObjectNames::Moon) {
				const auto x = earth.getGlobalPose().p - position;
				const auto magnitude = x.magnitude();
				const auto normalized = x / magnitude;
				rigidBody->addForce(UniversalGravitation::CalculateAccelerationMagnitude(earth.getMass(), magnitude) * normalized, PxForceMode::eACCELERATION);
			}

			if (const auto& star = *RigidActors.at(ObjectNames::Star);
				static_cast<bool>(star.userData) && renderObject.Name != ObjectNames::Star) {
				const auto x = star.getGlobalPose().p - position;
				const auto normalized = x.getNormalized();
				rigidBody->addForce(10.0f * normalized, PxForceMode::eACCELERATION);
			}
		}

		PhysX->Tick(static_cast<float>(min(1.0 / 60, elapsedSeconds)));
	}

private:
	bool m_isPhysXRunning = true;
};
}
