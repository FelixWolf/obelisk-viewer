#include "PointerController.hpp"

#include "viewer/core/Log.hpp"

namespace viewer
{

namespace
{

protocol::Vector3 toVector(const glm::vec3& v)
{
    return {v.x, v.y, v.z};
}

} // namespace

void PointerController::aim(render::Scene& scene, const std::optional<Ray>& ray)
{
    aimed_ = ray.has_value();
    if(!ray)
    {
        scene.pick.reset();
        hover_.reset();
        return;
    }
    render::PickRequest request;
    request.id = nextRequest_++;
    request.origin = ray->origin;
    request.direction = glm::normalize(ray->direction);
    request.hudPoint = ray->hudPoint;
    scene.pick = request;
    latestRequest_ = request.id;
}

void PointerController::update(render::Renderer& renderer, const world::World& world,
                               const ObjectView& objects)
{
    const auto result = renderer.takePick();
    if(!result || !aimed_)
    {
        return;
    }
    const auto picked = objects.resolvePick(result->pickId);
    const world::Object* object =
        picked ? world.object(picked->key.region, picked->key.localId) : nullptr;
    if(!object)
    {
        hover_.reset();
        return;
    }
    const world::Object& root = ObjectView::linkRoot(*object, world);
    Hover hover;
    hover.key = picked->key;
    hover.face = picked->face;
    hover.hud = result->hud;
    hover.touchable = ObjectView::touchable(*object, root);
    hover.clickAction = ObjectView::clickAction(*object, root);
    hover.hit = *result;
    hover_ = hover;
}

void PointerController::press(session::Session& session, const world::World& world,
                              const ObjectView& objects, const AvatarView* avatars,
                              uint64_t originHandle, uint64_t agentRegion)
{
    release(session);
    if(!hover_ || !hover_->touchable)
    {
        return;
    }
    const world::Object* object = world.object(hover_->key.region, hover_->key.localId);
    if(!object)
    {
        return;
    }
    if(hover_->clickAction != world::ClickAction::Touch)
    {
        // Sit, pay, buy, open, play, zoom: not yet; a touch meanwhile.
        log::info("click action {} isn't supported yet: touching instead", hover_->clickAction);
    }
    // From the prim to its root, in the root's frame (lltoolgrab.cpp).
    const world::Object& root = ObjectView::linkRoot(*object, world);
    const auto frame = objects.objectFrame(*object, world, originHandle, avatars);
    const auto rootFrame = objects.objectFrame(root, world, originHandle, avatars);
    glm::vec3 grabOffset(0.0f);
    if(frame && rootFrame)
    {
        const glm::mat3 rootRotation(*rootFrame);
        grabOffset =
            glm::transpose(rootRotation) * (glm::vec3((*rootFrame)[3]) - glm::vec3((*frame)[3]));
    }
    // Positions in the agent's region's coordinates (HUD hits stay in HUD
    // space).
    const render::PickResult& hit = hover_->hit;
    glm::vec3 position = hit.position;
    if(!hit.hud)
    {
        const glm::vec2 originCorner(static_cast<float>(originHandle >> 32),
                                     static_cast<float>(originHandle & 0xFFFFFFFFu));
        const glm::vec2 agentCorner(static_cast<float>(agentRegion >> 32),
                                    static_cast<float>(agentRegion & 0xFFFFFFFFu));
        position += glm::vec3(originCorner - agentCorner, 0.0f);
    }
    session::Session::TouchSurface surface;
    surface.uv = {hit.uv.x, hit.uv.y, 0.0f};
    surface.st = {hit.st.x, hit.st.y, 0.0f};
    surface.face = hover_->face;
    surface.position = toVector(position);
    surface.normal = toVector(hit.normal);
    surface.binormal = toVector(hit.binormal);
    session.touchObject(object->region, object->localId, toVector(grabOffset), surface);
    grabbed_ = Grab{object->region, object->localId, surface};
    log::info("touched {} (face {})", object->id.toString(), hover_->face);
}

void PointerController::release(session::Session& session)
{
    if(!grabbed_)
    {
        return;
    }
    session.releaseObject(grabbed_->region, grabbed_->localId, grabbed_->surface);
    grabbed_.reset();
}

} // namespace viewer
