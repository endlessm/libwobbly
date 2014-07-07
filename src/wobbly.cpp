/*
 * src/wobbly.cpp
 *
 * A spring model for implementing "wobbly" textures.
 *
 * This works by subdividing a texture's unit-coordinate-space
 * into a series of points, called "Objects". Object in turn, are
 * linked together into a mesh by springs, with each object being
 * connected bidirectionally by a spring both south and east of it,
 * for instance:
 *
 *    o(1)---s(1)---o(2)
 *    |               |
 *    |               |
 *    s(2)          s(4)
 *    |               |
 *    |               |
 *    o(3)---s(3)---o(4)
 *
 * The force delta applied to each spring at time t is
 *
 *     dF[t] = (distance * (o[a] - o[b])) * k
 *
 * This is in turn applied to the child objects
 *
 *     F[a] += -dF[t]
 *     F[b] += dF[t]
 *
 * Velocity at for a given time (t) is calculated by:
 *
 *     A[t] = (F/M)
 *     dV[t] = A[t] * dT
 *     V[t] += dV[t]
 *
 * We don't want infinite acceleration so we apply a friction vector
 * proportional to the velocity of the point at time (t)
 *
 *     Fr[t] = V[t] * Fk
 *
 * "Anchor" objects are special. No force can ever be applied to them,
 * and the feedback effect of this is that greater force will be
 * applied to its siblings.
 *
 * See LICENCE.md for Copyright information.
 *
 * The original implementation of this for Luminocity is
 * Copyright (c) Kristian Hoegsberg
 *
 * The original implementation of this for Compiz is
 * Copyright (c) 2005 David Reveman
 */
#include <limits>

/* boost/geometry/strategies/cartesian/distance_pythagoras depends on
 * boost/numeric/conversion/cast for boost::numeric_cast but does not
 * include it. See Boost bug #10177 */
#include <boost/numeric/conversion/cast.hpp>

#include <boost/geometry/algorithms/distance.hpp>
#include <boost/geometry/arithmetic/arithmetic.hpp>
#include <boost/geometry/strategies/cartesian/distance_pythagoras.hpp>
#include <boost/optional.hpp>

#include <smspillaz/wobbly/wobbly.h>

#include "wobbly_internal.h"

namespace bg = boost::geometry;

namespace
{
    size_t
    ObjectCountForGridSize (unsigned int width,
                            unsigned int height)
    {
        return width * height;
    }

    size_t
    SpringCountForGridSize (unsigned int width,
                            unsigned int height)
    {
        return ((width - 1) * (height - 1)) * 2 + width + height - 2;
    }
}

namespace wobbly
{
    struct Anchor::Private
    {
        Private (PointView <double> &&position,
                 Anchor::Storage    &anchors,
                 size_t             index);
        ~Private ();

        PointView <double>  position;
        Anchor::Storage     &anchors;
        size_t              index;
    };
}

wobbly::Anchor::Private::Private (PointView <double>      &&position,
                                  Storage                 &anchors,
                                  size_t                  index) :
    position (std::move (position)),
    anchors (anchors),
    index (index)
{
    anchors.Lock (index);
}

wobbly::Anchor::Anchor (PointView <double>      &&position,
                        Storage                 &anchors,
                        size_t                  index) :
    priv (new Private (std::move (position), anchors, index))
{
}

wobbly::Anchor::Anchor (Anchor &&grab) :
    priv (std::move (grab.priv))
{
}

wobbly::Anchor::Private::~Private ()
{
    anchors.Unlock (index);
}

wobbly::Anchor::~Anchor ()
{
}

void
wobbly::Anchor::MoveBy (Point const &delta)
{
    bg::add_point (priv->position, delta);
}

wobbly::Spring::Spring (PointView <double> &&forceA,
                        PointView <double> &&forceB,
                        PointView <double const> &&posA,
                        PointView <double const> &&posB,
                        Vector distance) :
    forceA (std::move (forceA)),
    forceB (std::move (forceB)),
    posA (std::move (posA)),
    posB (std::move (posB)),
    desiredDistance (distance)
{
}

wobbly::Spring::Spring (Spring &&spring) noexcept :
    forceA (std::move (spring.forceA)),
    forceB (std::move (spring.forceB)),
    posA (std::move (spring.posA)),
    posB (std::move (spring.posB)),
    desiredDistance (std::move (spring.desiredDistance))
{
}

wobbly::Spring::~Spring ()
{
}

void
wobbly::Spring::ScaleLength (Vector scaleFactor)
{
    bg::multiply_point (desiredDistance, scaleFactor);
}

wobbly::SpringMesh::SpringMesh (BezierMesh::MeshArray &points,
                                double                springWidth,
                                double                springHeight) :
    springWidth (springWidth),
    springHeight (springHeight)
{
    DistributeSprings (points, springWidth, springHeight);
}

void
wobbly::SpringMesh::DistributeSprings (BezierMesh::MeshArray &points,
                                       double                springWidth,
                                       double                springHeight)
{
    unsigned int const gridWidth = BezierMesh::Width;
    unsigned int const gridHeight = BezierMesh::Height;

    size_t const nSprings = SpringCountForGridSize (gridWidth, gridHeight);

    mSprings.clear ();
    mSprings.reserve (nSprings);

    for (size_t j = 0; j < gridHeight; ++j)
    {
        for (size_t i = 0; i < gridWidth; ++i)
        {
            typedef PointView <double> DPV;
            typedef PointView <double const> CDPV;

            size_t current = j * gridWidth + i;
            size_t below = (j + 1) * gridWidth + i;
            size_t right = j * gridWidth + i + 1;

            /* Spring from us to object below us */
            if (j < gridHeight - 1)
            {
                mSprings.emplace_back (DPV (mForces, current),
                                       DPV (mForces, below),
                                       CDPV (points, current),
                                       CDPV (points, below),
                                       Vector (0.0, springHeight));
            }

            /* Spring from us to object right of us */
            if (i < gridWidth - 1)
            {
                mSprings.emplace_back (DPV (mForces, current),
                                       DPV (mForces, right),
                                       CDPV (points, current),
                                       CDPV (points, right),
                                       Vector (springWidth, 0.0f));
            }
        }
    }

    assert (mSprings.size () == nSprings);
}

void
wobbly::SpringMesh::Scale (double x, double y)
{
    springWidth *= x;
    springHeight *= y;

    wobbly::Vector const scaleFactor (x, y);

    for (Spring &spring : mSprings)
        spring.ScaleLength (scaleFactor);
}

namespace wobbly
{
    class Model::Private
    {
        public:

            Private (Point const &initialPosition,
                     double      width,
                     double      height,
                     Settings    const &settings);

            std::array <wobbly::Point, 4> const
            Extremes () const;

            wobbly::Point
            TargetPosition () const;

            BezierMesh                    mPositions;
            BezierMesh::AnchorArray       mAnchors;
            EulerIntegration              mVelocityIntegrator;
            SpringStep <EulerIntegration> mSpring;
            ConstrainmentStep             mConstrainment;

            Model::Settings         const &mSettings;

            double mWidth, mHeight;
            bool mCurrentlyUnequal;
    };
}

namespace
{
    inline void
    CalculatePositionArray (wobbly::Point           const &initialPosition,
                            wobbly::BezierMesh::MeshArray &array,
                            size_t const                  objectsSize,
                            double const                  tileWidth,
                            double const                  tileHeight)
    {
        for (size_t i = 0; i < objectsSize; ++i)
        {
            size_t const row = i / wobbly::BezierMesh::Width;
            size_t const column = i % wobbly::BezierMesh::Width;

            wobbly::PointView <double> position (array, i);
            bg::assign (position, initialPosition);
            bg::add_point (position,
                wobbly::Point (column * tileWidth,
                               row * tileHeight));
        }
    }
}

wobbly::Model::Private::Private (Point const &initialPosition,
                                 double      width,
                                 double      height,
                                 Settings    const &settings) :
    mSpring (mVelocityIntegrator,
             mPositions.PointArray (),
             settings.springConstant,
             settings.friction,
             width / (BezierMesh::Width - 1),
             height / (BezierMesh::Height - 1)),
    mConstrainment (settings.maximumRange, width, height),
    mSettings (settings),
    mWidth (width),
    mHeight (height)
{
    size_t const objectsSize = ObjectCountForGridSize (BezierMesh::Width,
                                                       BezierMesh::Height);

    /* First construct the position array */
    CalculatePositionArray (initialPosition,
                            mPositions.PointArray (),
                            objectsSize,
                            width / (BezierMesh::Width - 1),
                            height / (BezierMesh::Height - 1));
}

wobbly::Model::Settings const wobbly::Model::DefaultSettings =
{
    wobbly::Model::DefaultSpringConstant,
    wobbly::Model::Friction,
    wobbly::Model::DefaultObjectRange
};

wobbly::Model::Model (Point const &initialPosition,
                      double      width,
                      double      height,
                      Settings    const &settings) :
    priv (new Private (initialPosition, width, height, settings))
{
}

wobbly::Model::Model (Point const &initialPosition,
                      double      width,
                      double      height) :
    priv (new Private (initialPosition, width, height, DefaultSettings))
{
}


wobbly::Model::~Model ()
{
}

namespace
{
    template <typename Integrator>
    bool PerformIntegration (wobbly::BezierMesh::MeshArray         &positions,
                             wobbly::BezierMesh::AnchorArray const &anchors,
                             Integrator                            &&integrator)
    {
        return integrator (positions, anchors);
    }

    template <typename Integrator, typename... Remaining>
    bool PerformIntegration (wobbly::BezierMesh::MeshArray         &positions,
                             wobbly::BezierMesh::AnchorArray const &anchors,
                             Integrator                            &&integrator,
                             Remaining&&...                        remaining)
    {
        bool more = integrator (positions, anchors);
        more |= PerformIntegration (positions,
                                    anchors,
                                    std::forward <Remaining> (remaining)...);
        return more;
    }

    template <typename... Args>
    bool Integrate (wobbly::BezierMesh::MeshArray         &positions,
                    wobbly::BezierMesh::AnchorArray const &anchors,
                    unsigned int                          steps,
                    Args&&                                ...integrators)
    {
        bool more = false;

        /* Force is actually going to be something that changes over time
         * depending on how far about the objects are away from each other.
         *
         * Unfortunately that's not a simple thing to model. It requires us to
         * provide an integration of the force function which is in turn
         * dependent on the position of the objects which are in turn dependent
         * on the force applied. That requires implicit integration, which is
         * not dynamic.
         *
         * Its far easier to simply just approximate this by sampling the
         * integration function. The problem that you end up facing then is that
         * for large enough stepsizes (where stepsize > 2 * friction/ k) then
         * you end up with model instability.
         *
         * Having one step every frame is a good approximation although that
         * might need to change in the future */
        while (steps--)
            more |= PerformIntegration (positions,
                                        anchors,
                                        std::forward <Args> (integrators)...);

        return more;
    }

    template <typename AnchorPoint>
    wobbly::Point TopLeftPositionInSettledMesh (AnchorPoint const &anchor,
                                                size_t      const index,
                                                double      const tileWidth,
                                                double      const tileHeight)
    {
        wobbly::Point start;
        bg::assign_point (start, anchor);

        wobbly::Point deltaToTopLeft (tileWidth *
                                          (index % wobbly::BezierMesh::Width),
                                      tileHeight *
                                          (index / wobbly::BezierMesh::Width));
        bg::subtract_point (start, deltaToTopLeft);

        return start;
    }
}

wobbly::Point
wobbly::Model::Private::TargetPosition () const
{
    double const tileW = mWidth / (BezierMesh::Width - 1);
    double const tileH = mHeight / (BezierMesh::Height - 1);

    auto points (mPositions.PointArray ());
    auto anchors (mAnchors);

    /* If we have at least one anchor, we can take a short-cut and determine
     * the target position by reference to it */
    boost::optional <wobbly::Point> early;
    mAnchors.WithFirstGrabbed ([&early, &points, tileW, tileH] (size_t index) {
                                    auto const anchor =
                                        wobbly::PointView <double> (points,
                                                                    index);

                                    early =
                                        TopLeftPositionInSettledMesh (anchor,
                                                                      index,
                                                                      tileW,
                                                                      tileH);
                               });

    if (early.is_initialized ())
        return early.get ();

    /* Make our own copies of the integrators and run the integration on them */
    EulerIntegration              integrator (mVelocityIntegrator);
    SpringStep <EulerIntegration> spring (integrator,
                                          points,
                                          mSettings.springConstant,
                                          mSettings.friction,
                                          tileW,
                                          tileH);
    ConstrainmentStep             constrainment (mConstrainment);

    /* Keep on integrating this copy until we know the final position */
    while (Integrate (points, anchors, 1, spring, constrainment));

    /* Model will be settled, return the top left point */
    wobbly::Point result;
    bg::assign_point (result, wobbly::PointView <double> (points, 0));

    return result;
}

namespace
{
    size_t
    ClosestIndexToAbsolutePosition (size_t                        count,
                                    wobbly::BezierMesh::MeshArray &points,
                                    wobbly::Point                 const &pos)
    {
        boost::optional <size_t> nearestIndex;
        double distance = std::numeric_limits <double>::max ();

        for (size_t i = 0; i < count; ++i)
        {
            wobbly::PointView <double> view (points, i);
            double objectDistance = std::fabs (bg::distance (pos, view));
            if (objectDistance < distance)
            {
                nearestIndex = i;
                distance = objectDistance;
            }
        }

        assert (nearestIndex.is_initialized ());
        return nearestIndex.get ();
    }
}

wobbly::Anchor
wobbly::Model::GrabAnchor (Point const &position)
{
    auto &points = priv->mPositions.PointArray ();
    size_t count = ObjectCountForGridSize (BezierMesh::Width,
                                           BezierMesh::Height);
    size_t index = ClosestIndexToAbsolutePosition (count,
                                                   points,
                                                   position);

    /* Bets are off once we've grabbed an anchor, the model is now unequal */
    priv->mCurrentlyUnequal = true;

    /* Set up grab notification */
    return Anchor (wobbly::PointView <double> (points, index),
                   priv->mAnchors,
                   index);
}

void
wobbly::Model::MoveModelBy (Point const &delta)
{
    auto &points (priv->mPositions.PointArray ());
    size_t count = ObjectCountForGridSize (BezierMesh::Width,
                                           BezierMesh::Height);
    for (size_t i = 0; i < count; ++i)
    {
        PointView <double> pv (points, i);
        bg::add_point (pv, delta);
    }
}

void
wobbly::Model::MoveModelTo (Point const &point)
{
    /* We need to calculate the target position for the
     * top left corner. If we do that, then moving the model
     * relative to that will ensure that it settles in the
     * place that we expect. */

    auto const &target (priv->TargetPosition ());

    Vector delta (point);
    bg::subtract_point (delta, target);

    MoveModelBy (delta);
}

void
wobbly::Model::ResizeModel (double width, double height)
{
    /* First, zero or negative widths are invalid */
    assert (width > 0.0f);
    assert (height > 0.0f);

    /* Second, work out the scale factors */
    double const scaleFactorX = width / priv->mWidth;
    double const scaleFactorY = height / priv->mHeight;

    wobbly::Vector const scaleFactor (scaleFactorX, scaleFactorY);

    if (bg::equals (scaleFactor, wobbly::Vector (1.0, 1.0)))
        return;

    wobbly::Point const modelTargetOrigin (priv->TargetPosition ());

    /* Then on each point, implement a transformation
     * for non-anchors that scales the distance between
     * points in model space */
    auto &points (priv->mPositions.PointArray ());

    auto rescaleAction =
        [&points, &modelTargetOrigin, &scaleFactor](size_t index) {
            wobbly::PointView <double> p (points, index);
            bg::subtract_point (p, modelTargetOrigin);
            bg::multiply_point (p, scaleFactor);
            bg::add_point (p, modelTargetOrigin);
        };

    priv->mAnchors.PerformActions ([](size_t index) {
                                   },
                                   rescaleAction);

    /* On each spring, apply the scale factor */
    priv->mSpring.Scale (scaleFactorX, scaleFactorY);

    /* Apply width and height changes */
    priv->mWidth = width;
    priv->mHeight = height;
}

wobbly::ConstrainmentStep::ConstrainmentStep (double const &threshold,
                                              double const &width,
                                              double const &height) :
    threshold (threshold),
    mWidth (width),
    mHeight (height)
{
    targetBuffer.fill (0.0);
}

bool
wobbly::ConstrainmentStep::operator () (BezierMesh::MeshArray         &points,
                                        BezierMesh::AnchorArray const &anchors)
{
    bool ret = false;
    /* If an anchor is grabbed, then the model will be considered constrained.
     * The first anchor taking priority - we work out the allowable range for
     * each spring and then apply correction as appropriate before even
     * starting to integrate the model
     */
    auto const action =
        [this, &points, &ret](size_t index) {
            double const tileWidth = mWidth / (BezierMesh::Width - 1);
            double const tileHeight = mHeight / (BezierMesh::Height - 1);
            auto const anchor = wobbly::PointView <double> (points, index);

            wobbly::Point start (TopLeftPositionInSettledMesh (anchor,
                                                               index,
                                                               tileWidth,
                                                               tileHeight));

            size_t const objectsSize =
                ObjectCountForGridSize (BezierMesh::Width,
                                        BezierMesh::Height);

            targetBuffer.fill (0.0);
            CalculatePositionArray (start,
                                    targetBuffer,
                                    objectsSize,
                                    tileWidth,
                                    tileHeight);

            /* In each position in the main position array we'll work out the
             * pythagorean delta between the ideal positon and current one.
             * If it is outside the maximum range, then we'll shrink the delta
             * and reapply it */
            double const maximumRange = threshold;

            for (size_t i = 0; i < objectsSize; ++i)
            {
                wobbly::PointView <double> point (points, i);
                wobbly::PointView <double> target (targetBuffer, i);

                auto range = bg::distance (point, target);

                if (range < maximumRange)
                    continue;

                ret |= true;

                auto sin = (bg::get <1> (point) - bg::get <1> (target)) / range;
                auto cos = (bg::get <0> (point) - bg::get <0> (target)) / range;

                /* Now we want to reduce range and
                 * find our new x and y offsets */
                range = std::min (maximumRange, range);

                wobbly::Point newDelta (range * cos, range * sin);
                bg::assign_point (point, target);
                bg::subtract_point (point, newDelta);
            }
        };

    anchors.WithFirstGrabbed (action);

    return ret;
}

wobbly::EulerIntegration::EulerIntegration ()
{
    velocities.fill (0.0);
}

bool
wobbly::Model::Step (unsigned int time)
{
    bool moreStepsRequired = priv->mCurrentlyUnequal;

    double const FPStepResolution = 16.0f;
    unsigned int steps =
        static_cast <unsigned int> (std::ceil (time / FPStepResolution));

    /* We might not need more steps - set to false initially and then
     * integrate the model to see if we do */
    if (time)
        moreStepsRequired = false;

    moreStepsRequired |= Integrate (priv->mPositions.PointArray (),
                                    priv->mAnchors,
                                    steps,
                                    priv->mConstrainment,
                                    priv->mSpring);

    priv->mCurrentlyUnequal = moreStepsRequired;

    /* If we've settled and have grabbed anchors, snap to the mesh resting
     * point where the cursor is, this ensures exact positioning */
    if (!priv->mCurrentlyUnequal)
    {
        auto       &positions (priv->mPositions.PointArray ());
        auto const &anchors (priv->mAnchors);

        auto const action =
            [this, &positions](size_t index) {
                double const tileW = priv->mWidth / (BezierMesh::Width - 1);
                double const tileH = priv->mHeight / (BezierMesh::Height - 1);
                size_t const count =
                    ObjectCountForGridSize (BezierMesh::Width,
                                            BezierMesh::Height);

                auto const anchor = wobbly::PointView <double> (positions,
                                                                index);

                auto const tl (TopLeftPositionInSettledMesh (anchor,
                                                             index,
                                                             tileW,
                                                             tileH));

                CalculatePositionArray (tl,
                                        positions,
                                        count,
                                        tileW,
                                        tileH);
            };
        anchors.WithFirstGrabbed (action);
    }

    return priv->mCurrentlyUnequal;
}

wobbly::Point
wobbly::Model::DeformTexcoords (Point const &normalized) const
{
    return priv->mPositions.DeformUnitCoordsToMeshSpace (normalized);
}

std::array <wobbly::Point, 4> const
wobbly::Model::Extremes () const
{
    return priv->mPositions.Extremes ();
}

wobbly::BezierMesh::BezierMesh ()
{
    mPoints.fill (0.0);
}

wobbly::BezierMesh::~BezierMesh ()
{
}

namespace
{
    class PointRoundOperation
    {
        public:

            template <typename P, int I>
            void apply (P &p) const
            {
                bg::set <I> (p, std::round (bg::get <I> (p)));
            }
    };

    template <typename Point>
    void PointRound (Point &point)
    {
        bg::for_each_coordinate (point, PointRoundOperation ());
    }

    template <typename Numeric, typename Minimum, typename Maximum>
    void SetToExtreme (wobbly::Point  &p,
                       Numeric        x,
                       Minimum        xFinder,
                       Numeric        y,
                       Maximum        yFinder)
    {
        bg::set <0> (p, xFinder (bg::get <0> (p), x));
        bg::set <1> (p, yFinder (bg::get <1> (p), y));

        /* Round to next integer */
        PointRound (p);
    }

    unsigned int CoordIndex (size_t x,
                             size_t y,
                             unsigned int width)
    {
        return y * width + x;
    }
}

std::array <wobbly::Point, 4> const
wobbly::BezierMesh::Extremes () const
{
    double const maximum = std::numeric_limits <double>::max ();
    double const minimum = std::numeric_limits <double>::lowest ();

    std::array <wobbly::Point, 4> extremes =
    {
        {
            wobbly::Point (maximum, maximum),
            wobbly::Point (minimum, maximum),
            wobbly::Point (maximum, minimum),
            wobbly::Point (minimum, minimum)
        }
    };

    wobbly::Point &topLeft (extremes[0]);
    wobbly::Point &topRight (extremes[1]);
    wobbly::Point &bottomLeft (extremes[2]);
    wobbly::Point &bottomRight (extremes[3]);

    auto min = [](double lhs, double rhs) -> double {
        double result =  std::min (lhs, rhs);
        return result;
    };

    auto max = [](double lhs, double rhs) -> double {
        double result = std::max (lhs, rhs);
        return result;
    };

    for (size_t i = 0; i < Width * Height * 2; i += 2)
    {
        double const x = mPoints[i];
        double const y = mPoints[i + 1];

        SetToExtreme (topLeft, x, min, y, min);
        SetToExtreme (topRight, x, max, y, min);
        SetToExtreme (bottomLeft, x, min, y, max);
        SetToExtreme (bottomRight, x, max, y, max);
    }

    return extremes;
}

wobbly::PointView <double>
wobbly::BezierMesh::PointForIndex (size_t x, size_t y)
{
    return wobbly::PointView <double> (mPoints,
                                       CoordIndex (x, y,
                                                   BezierMesh::Width));
}

wobbly::PointView <double const>
wobbly::BezierMesh::PointForIndex (size_t x, size_t y) const
{
    return wobbly::PointView <double const> (mPoints,
                                             CoordIndex (x, y,
                                                         BezierMesh::Width));
}
