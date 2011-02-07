/* mx-kinetic-scroll-view.c: Kinetic scrolling container actor
 *
 * Copyright (C) 2008 OpenedHand
 * Copyright (C) 2010 Intel Corporation.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St - Fifth Floor, Boston, MA 02110-1301 USA.
 * Boston, MA 02111-1307, USA.
 *
 * Written by: Chris Lord <chris@linux.intel.com>
 */

/**
 * SECTION:mx-kinetic-scroll-view
 * @short_description: A kinetic scrolling container widget
 *
 * #MxKineticScrollView is a single child container for actors that implements
 * #MxScrollable. It allows the contained child to be dragged to scroll, and
 * maintains the momentum once the drag is complete. Deceleration after
 * dragging is configurable, and it will always snap to the
 * #MxAdjustment:step-increment boundary.
 *
 * #MxKineticScrollView also implements #MxScrollable itself, allowing it to
 * be embedded in an #MxScrollView to provide scroll-bars.
 */

#include "mx-kinetic-scroll-view.h"
#include "mx-enum-types.h"
#include "mx-marshal.h"
#include "mx-private.h"
#include "mx-scrollable.h"
#include <math.h>

static void mx_scrollable_iface_init (MxScrollableIface *iface);

G_DEFINE_TYPE_WITH_CODE (MxKineticScrollView,
                         mx_kinetic_scroll_view, MX_TYPE_BIN,
                         G_IMPLEMENT_INTERFACE (MX_TYPE_SCROLLABLE,
                                                mx_scrollable_iface_init))

#define KINETIC_SCROLL_VIEW_PRIVATE(o) (G_TYPE_INSTANCE_GET_PRIVATE ((o), \
                                        MX_TYPE_KINETIC_SCROLL_VIEW, \
                                        MxKineticScrollViewPrivate))

typedef struct {
  /* Units to store the origin of a click when scrolling */
  gfloat   x;
  gfloat   y;
  GTimeVal time;
} MxKineticScrollViewMotion;

struct _MxKineticScrollViewPrivate
{
  ClutterActor          *child;

  guint                  use_captured : 1;
  guint                  in_drag      : 1;
  guint                  hmoving      : 1;
  guint                  vmoving      : 1;
  guint32                button;

  /* Mouse motion event information */
  GArray                *motion_buffer;
  guint                  last_motion;

  /* Variables for storing acceleration information */
  ClutterTimeline       *deceleration_timeline;
  gfloat                 dx;
  gfloat                 dy;
  gdouble                decel_rate;
  gdouble                overshoot;
  gdouble                accumulated_delta;
};

enum {
  PROP_0,

  PROP_DECELERATION,
/*  PROP_BUFFER_SIZE,*/
  PROP_HADJUST,
  PROP_VADJUST,
  PROP_BUTTON,
  PROP_USE_CAPTURED,
  PROP_OVERSHOOT
};

/* MxScrollableIface implementation */

static void
mx_kinetic_scroll_view_set_adjustments (MxScrollable *scrollable,
                                        MxAdjustment *hadjustment,
                                        MxAdjustment *vadjustment)
{
  MxKineticScrollViewPrivate *priv = MX_KINETIC_SCROLL_VIEW (scrollable)->priv;

  if (priv->child)
    mx_scrollable_set_adjustments (MX_SCROLLABLE (priv->child),
                                   hadjustment,
                                   vadjustment);
}

static void
mx_kinetic_scroll_view_get_adjustments (MxScrollable  *scrollable,
                                        MxAdjustment **hadjustment,
                                        MxAdjustment **vadjustment)
{
  MxKineticScrollViewPrivate *priv = MX_KINETIC_SCROLL_VIEW (scrollable)->priv;

  if (priv->child)
    {
      mx_scrollable_get_adjustments (MX_SCROLLABLE (priv->child),
                                     hadjustment,
                                     vadjustment);
    }
  else
    {
      if (hadjustment)
        *hadjustment = NULL;
      if (vadjustment)
        *vadjustment = NULL;
    }
}

static void
mx_scrollable_iface_init (MxScrollableIface *iface)
{
  iface->set_adjustments = mx_kinetic_scroll_view_set_adjustments;
  iface->get_adjustments = mx_kinetic_scroll_view_get_adjustments;
}

/* Object implementation */

static void
mx_kinetic_scroll_view_get_property (GObject    *object,
                                     guint       property_id,
                                     GValue     *value,
                                     GParamSpec *pspec)
{
  MxAdjustment *adjustment;
  MxKineticScrollViewPrivate *priv = MX_KINETIC_SCROLL_VIEW (object)->priv;

  switch (property_id)
    {
    case PROP_DECELERATION :
      g_value_set_double (value, priv->decel_rate);
      break;

/*
    case PROP_BUFFER_SIZE :
      g_value_set_uint (value, priv->motion_buffer->len);
      break;
*/

    case PROP_HADJUST:
      mx_kinetic_scroll_view_get_adjustments (MX_SCROLLABLE (object),
                                        &adjustment, NULL);
      g_value_set_object (value, adjustment);
      break;

    case PROP_VADJUST:
      mx_kinetic_scroll_view_get_adjustments (MX_SCROLLABLE (object),
                                        NULL, &adjustment);
      g_value_set_object (value, adjustment);
      break;

    case PROP_BUTTON:
      g_value_set_uint (value, priv->button);
      break;

    case PROP_USE_CAPTURED:
      g_value_set_boolean (value, priv->use_captured);
      break;

    case PROP_OVERSHOOT:
      g_value_set_double (value, priv->overshoot);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
    }
}

static void
mx_kinetic_scroll_view_set_property (GObject      *object,
                                     guint         property_id,
                                     const GValue *value,
                                     GParamSpec   *pspec)
{
  MxAdjustment *adjustment;
  MxScrollable *scrollable;
  MxKineticScrollView *self = MX_KINETIC_SCROLL_VIEW (object);

  switch (property_id)
    {
    case PROP_DECELERATION :
      mx_kinetic_scroll_view_set_deceleration (self,
                                               g_value_get_double (value));
      break;

/*
    case PROP_BUFFER_SIZE :
      mx_kinetic_scroll_view_set_buffer_size (self, g_value_get_uint (value));
      break;
*/

    case PROP_HADJUST:
      scrollable = MX_SCROLLABLE (object);
      mx_kinetic_scroll_view_get_adjustments (scrollable, NULL, &adjustment);
      mx_kinetic_scroll_view_set_adjustments (scrollable,
                                        g_value_get_object (value),
                                        adjustment);
      break;

    case PROP_VADJUST:
      scrollable = MX_SCROLLABLE (object);
      mx_kinetic_scroll_view_get_adjustments (scrollable, &adjustment, NULL);
      mx_kinetic_scroll_view_set_adjustments (scrollable,
                                        adjustment,
                                        g_value_get_object (value));
      break;

    case PROP_BUTTON:
      mx_kinetic_scroll_view_set_mouse_button (self, g_value_get_uint (value));
      break;

    case PROP_USE_CAPTURED:
      mx_kinetic_scroll_view_set_use_captured (self,
                                               g_value_get_boolean (value));
      break;

    case PROP_OVERSHOOT:
      mx_kinetic_scroll_view_set_overshoot (self, g_value_get_double (value));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
    }
}

static void
mx_kinetic_scroll_view_dispose (GObject *object)
{
  MxKineticScrollViewPrivate *priv = MX_KINETIC_SCROLL_VIEW (object)->priv;

  if (priv->deceleration_timeline)
    {
      clutter_timeline_stop (priv->deceleration_timeline);
      g_object_unref (priv->deceleration_timeline);
      priv->deceleration_timeline = NULL;
    }

  G_OBJECT_CLASS (mx_kinetic_scroll_view_parent_class)->dispose (object);
}

static void
mx_kinetic_scroll_view_finalize (GObject *object)
{
  MxKineticScrollViewPrivate *priv = MX_KINETIC_SCROLL_VIEW (object)->priv;

  g_array_free (priv->motion_buffer, TRUE);

  G_OBJECT_CLASS (mx_kinetic_scroll_view_parent_class)->finalize (object);
}

static void
mx_kinetic_scroll_view_get_preferred_width (ClutterActor *actor,
                                            gfloat        for_height,
                                            gfloat       *min_width_p,
                                            gfloat       *nat_width_p)
{
  CLUTTER_ACTOR_CLASS (mx_kinetic_scroll_view_parent_class)->
    get_preferred_width (actor, for_height, NULL, nat_width_p);

  if (min_width_p)
    {
      MxPadding padding;

      mx_widget_get_padding (MX_WIDGET (actor), &padding);
      *min_width_p = padding.left + padding.right;
    }
}

static void
mx_kinetic_scroll_view_get_preferred_height (ClutterActor *actor,
                                             gfloat        for_width,
                                             gfloat       *min_height_p,
                                             gfloat       *nat_height_p)
{
  CLUTTER_ACTOR_CLASS (mx_kinetic_scroll_view_parent_class)->
    get_preferred_height (actor, for_width, NULL, nat_height_p);

  if (min_height_p)
    {
      MxPadding padding;

      mx_widget_get_padding (MX_WIDGET (actor), &padding);
      *min_height_p = padding.top + padding.bottom;
    }
}

static void
mx_kinetic_scroll_view_allocate (ClutterActor           *actor,
                                 const ClutterActorBox  *box,
                                 ClutterAllocationFlags  flags)
{
  CLUTTER_ACTOR_CLASS (mx_kinetic_scroll_view_parent_class)->
    allocate (actor, box, flags);

  mx_bin_allocate_child (MX_BIN (actor), box, flags);
}

static void
mx_kinetic_scroll_view_class_init (MxKineticScrollViewClass *klass)
{
  GParamSpec *pspec;

  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  ClutterActorClass *actor_class = CLUTTER_ACTOR_CLASS (klass);

  g_type_class_add_private (klass, sizeof (MxKineticScrollViewPrivate));

  object_class->get_property = mx_kinetic_scroll_view_get_property;
  object_class->set_property = mx_kinetic_scroll_view_set_property;
  object_class->dispose = mx_kinetic_scroll_view_dispose;
  object_class->finalize = mx_kinetic_scroll_view_finalize;

  actor_class->get_preferred_width =
    mx_kinetic_scroll_view_get_preferred_width;
  actor_class->get_preferred_height =
    mx_kinetic_scroll_view_get_preferred_height;
  actor_class->allocate =
    mx_kinetic_scroll_view_allocate;

  pspec = g_param_spec_double ("deceleration",
                               "Deceleration",
                               "Rate at which the view will decelerate in.",
                               1.1, G_MAXDOUBLE, 1.1,
                               MX_PARAM_READWRITE);
  g_object_class_install_property (object_class, PROP_DECELERATION, pspec);

  /*
  pspec = g_param_spec_uint ("buffer-size",
                             "Buffer size",
                             "Amount of motion events to buffer",
                             1, G_MAXUINT, 3,
                             MX_PARAM_READWRITE);
  g_object_class_install_property (object_class, PROP_BUFFER_SIZE, pspec);
  */

  pspec = g_param_spec_uint ("mouse-button",
                             "Mouse button",
                             "The mouse button used to control scrolling",
                             0, G_MAXUINT, 1,
                             MX_PARAM_READWRITE);
  g_object_class_install_property (object_class, PROP_BUTTON, pspec);

  pspec = g_param_spec_boolean ("use-captured",
                                "Use captured",
                                "Use captured events to initiate scrolling",
                                FALSE,
                                MX_PARAM_READWRITE);
  g_object_class_install_property (object_class, PROP_USE_CAPTURED, pspec);

  pspec = g_param_spec_double ("overshoot",
                               "Overshoot",
                               "The rate at which the view will decelerate "
                               "when scrolled beyond its boundaries.",
                               0.0, 1.0, 0.0,
                               MX_PARAM_READWRITE);
  g_object_class_install_property (object_class, PROP_OVERSHOOT, pspec);

  /* MxScrollable properties */
  g_object_class_override_property (object_class,
                                    PROP_HADJUST,
                                    "horizontal-adjustment");

  g_object_class_override_property (object_class,
                                    PROP_VADJUST,
                                    "vertical-adjustment");
}

static gboolean
motion_event_cb (ClutterActor        *stage,
                 ClutterMotionEvent  *event,
                 MxKineticScrollView *scroll)
{
  gfloat x, y;

  MxKineticScrollViewPrivate *priv = scroll->priv;
  ClutterActor *actor = CLUTTER_ACTOR (scroll);

  if (event->type != CLUTTER_MOTION)
    return FALSE;

  if (clutter_actor_transform_stage_point (actor,
                                           event->x,
                                           event->y,
                                           &x, &y))
    {
      MxKineticScrollViewMotion *motion;
      ClutterActor *child = mx_bin_get_child (MX_BIN (scroll));

      /* Check if we've passed the drag threshold */
      if (!priv->in_drag)
        {
          guint threshold;
          MxSettings *settings = mx_settings_get_default ();

          g_object_get (G_OBJECT (settings),
                        "drag-threshold", &threshold, NULL);
          motion = &g_array_index (priv->motion_buffer,
                                   MxKineticScrollViewMotion, 0);

          if ((ABS (motion->x - x) >= threshold) ||
              (ABS (motion->y - y) >= threshold))
            {
              clutter_set_motion_events_enabled (TRUE);
              priv->in_drag = TRUE;
            }
          else
            return FALSE;
        }

      if (child)
        {
          gdouble dx, dy;
          MxAdjustment *hadjust, *vadjust;

          mx_scrollable_get_adjustments (MX_SCROLLABLE (child),
                                         &hadjust, &vadjust);

          motion = &g_array_index (priv->motion_buffer,
                                   MxKineticScrollViewMotion,
                                   priv->last_motion);

          if (hadjust)
            {
              dx = (motion->x - x) + mx_adjustment_get_value (hadjust);
              mx_adjustment_set_value (hadjust, dx);
            }

          if (vadjust)
            {
              dy = (motion->y - y) + mx_adjustment_get_value (vadjust);
              mx_adjustment_set_value (vadjust, dy);
            }
        }

      priv->last_motion ++;
      if (priv->last_motion == priv->motion_buffer->len)
        {
          priv->motion_buffer = g_array_remove_index (priv->motion_buffer, 0);
          g_array_set_size (priv->motion_buffer, priv->last_motion);
          priv->last_motion --;
        }

      motion = &g_array_index (priv->motion_buffer,
                               MxKineticScrollViewMotion, priv->last_motion);
      motion->x = x;
      motion->y = y;
      g_get_current_time (&motion->time);
    }

  return TRUE;
}

static void
clamp_adjustments (MxKineticScrollView *scroll,
                   guint                duration,
                   gboolean             horizontal,
                   gboolean             vertical)
{
  ClutterActor *child = mx_bin_get_child (MX_BIN (scroll));

  if (child)
    {
      gdouble d, value, lower, upper, step_increment, page_size;
      MxAdjustment *hadj, *vadj;

      mx_scrollable_get_adjustments (MX_SCROLLABLE (child),
                                     &hadj, &vadj);

      if (horizontal && hadj)
        {
          /* Snap to the nearest step increment on hadjustment */
          mx_adjustment_get_values (hadj, &value, &lower, &upper,
                                    &step_increment, NULL, &page_size);
          d = (rint ((value - lower) / step_increment) *
              step_increment) + lower;
          d = CLAMP (d, lower, upper - page_size);
          mx_adjustment_interpolate (hadj, d, duration, CLUTTER_EASE_OUT_QUAD);
        }

      if (vertical && vadj)
        {
          /* Snap to the nearest step increment on vadjustment */
          mx_adjustment_get_values (vadj, &value, &lower, &upper,
                                    &step_increment, NULL, &page_size);
          d = (rint ((value - lower) / step_increment) *
              step_increment) + lower;
          d = CLAMP (d, lower, upper - page_size);
          mx_adjustment_interpolate (vadj, d, duration, CLUTTER_EASE_OUT_QUAD);
        }
    }
}

static void
deceleration_completed_cb (ClutterTimeline     *timeline,
                           MxKineticScrollView *scroll)
{
  MxKineticScrollViewPrivate *priv = scroll->priv;

  clamp_adjustments (scroll, (priv->overshoot > 0.0) ? 250 : 10,
                     priv->hmoving, priv->vmoving);

  g_object_unref (timeline);
  priv->deceleration_timeline = NULL;
}

static void
deceleration_new_frame_cb (ClutterTimeline     *timeline,
                           gint                 frame_num,
                           MxKineticScrollView *scroll)
{
  MxKineticScrollViewPrivate *priv = scroll->priv;
  ClutterActor *child = mx_bin_get_child (MX_BIN (scroll));

  if (child)
    {
      MxAdjustment *hadjust, *vadjust;

      gboolean stop = TRUE;

      mx_scrollable_get_adjustments (MX_SCROLLABLE (child),
                                     &hadjust, &vadjust);

      priv->accumulated_delta += clutter_timeline_get_delta (timeline);

      if (priv->accumulated_delta <= 1000.0/60.0)
        stop = FALSE;

      while (priv->accumulated_delta > 1000.0/60.0)
        {
          gdouble hvalue, vvalue;

          if (hadjust)
            {
              if (ABS (priv->dx) > 0.1)
                {
                  hvalue = priv->dx + mx_adjustment_get_value (hadjust);
                  mx_adjustment_set_value (hadjust, hvalue);

                  if (priv->overshoot > 0.0)
                    {
                      if ((hvalue > mx_adjustment_get_upper (hadjust) -
                           mx_adjustment_get_page_size (hadjust)) ||
                          (hvalue < mx_adjustment_get_lower (hadjust)))
                        priv->dx *= priv->overshoot;
                    }

                  priv->dx = priv->dx / priv->decel_rate;

                  stop = FALSE;
                }
              else if (priv->hmoving)
                {
                  priv->hmoving = FALSE;
                  clamp_adjustments (scroll,
                                     (priv->overshoot > 0.0) ? 250 : 10,
                                     TRUE, FALSE);
                }
            }

          if (vadjust)
            {
              if (ABS (priv->dy) > 0.1)
                {
                  vvalue = priv->dy + mx_adjustment_get_value (vadjust);
                  mx_adjustment_set_value (vadjust, vvalue);

                  if (priv->overshoot > 0.0)
                    {
                      if ((vvalue > mx_adjustment_get_upper (vadjust) -
                           mx_adjustment_get_page_size (vadjust)) ||
                          (vvalue < mx_adjustment_get_lower (vadjust)))
                        priv->dy *= priv->overshoot;
                    }

                  priv->dy = priv->dy / priv->decel_rate;

                  stop = FALSE;
                }
              else if (priv->vmoving)
                {
                  priv->vmoving = FALSE;
                  clamp_adjustments (scroll,
                                     (priv->overshoot > 0.0) ? 250 : 10,
                                     FALSE, TRUE);
                }
            }

          priv->accumulated_delta -= 1000.0/60.0;
        }

      if (stop)
        {
          clutter_timeline_stop (timeline);
          deceleration_completed_cb (timeline, scroll);
        }
    }
}

static gboolean
button_release_event_cb (ClutterActor        *stage,
                         ClutterButtonEvent  *event,
                         MxKineticScrollView *scroll)
{
  MxKineticScrollViewPrivate *priv = scroll->priv;
  ClutterActor *actor = CLUTTER_ACTOR (scroll);
  ClutterActor *child = mx_bin_get_child (MX_BIN (scroll));
  gboolean decelerating = FALSE;

  if ((event->type != CLUTTER_BUTTON_RELEASE) ||
      (event->button != priv->button))
    return FALSE;

  g_signal_handlers_disconnect_by_func (stage,
                                        motion_event_cb,
                                        scroll);
  g_signal_handlers_disconnect_by_func (stage,
                                        button_release_event_cb,
                                        scroll);

  if (!priv->in_drag)
    return FALSE;

  clutter_set_motion_events_enabled (TRUE);

  if (child)
    {
      gfloat event_x, event_y;

      if (clutter_actor_transform_stage_point (actor, event->x, event->y,
                                               &event_x, &event_y))
        {
          gdouble value, lower, upper, step_increment, page_size,
                  d, ax, ay, y, nx, ny, n;
          gfloat frac, x_origin, y_origin;
          GTimeVal release_time, motion_time;
          MxAdjustment *hadjust, *vadjust;
          glong time_diff;
          guint duration;
          gint i;

          /* Get time delta */
          g_get_current_time (&release_time);

          /* Get average position/time of last x mouse events */
          priv->last_motion ++;
          x_origin = y_origin = 0;
          motion_time = (GTimeVal){ 0, 0 };
          for (i = 0; i < priv->last_motion; i++)
            {
              MxKineticScrollViewMotion *motion =
                &g_array_index (priv->motion_buffer, MxKineticScrollViewMotion, i);

              /* FIXME: This doesn't guard against overflows - Should
               *        either fix that, or calculate the correct maximum
               *        value for the buffer size
               */
              x_origin += motion->x;
              y_origin += motion->y;
              motion_time.tv_sec += motion->time.tv_sec;
              motion_time.tv_usec += motion->time.tv_usec;
            }
          x_origin = x_origin / priv->last_motion;
          y_origin = y_origin / priv->last_motion;
          motion_time.tv_sec /= priv->last_motion;
          motion_time.tv_usec /= priv->last_motion;

          if (motion_time.tv_sec == release_time.tv_sec)
            time_diff = release_time.tv_usec - motion_time.tv_usec;
          else
            time_diff = release_time.tv_usec +
                        (G_USEC_PER_SEC - motion_time.tv_usec);

          /* Work out the fraction of 1/60th of a second that has elapsed */
          frac = (time_diff/1000.0) / (1000.0/60.0);

          /* See how many units to move in 1/60th of a second */
          priv->dx = (x_origin - event_x) / frac;
          priv->dy = (y_origin - event_y) / frac;

          /* If the delta is too low for the equations to work,
           * bump the values up a bit.
           */
          if (ABS (priv->dx) < 1)
            priv->dx = (priv->dx > 0) ? 1 : -1;
          if (ABS (priv->dy) < 1)
            priv->dy = (priv->dy > 0) ? 1 : -1;

          /* We want n, where x / y^n < z,
           * x = Distance to move per frame
           * y = Deceleration rate
           * z = maximum distance from target
           *
           * Rearrange to n = log (x / z) / log (y)
           * To simplify, z = 1, so n = log (x) / log (y)
           */
          y = priv->decel_rate;
          nx = logf (ABS (priv->dx)) / logf (y);
          ny = logf (ABS (priv->dy)) / logf (y);
          n = MAX (nx, ny);

          duration = MAX (1, (gint)(MAX (nx, ny) * (1000/60.0)));

          if (duration > 250)
            {
              /* Now we have n, adjust dx/dy so that we finish on a step
               * boundary.
               *
               * Distance moved, using the above variable names:
               *
               * d = x + x/y + x/y^2 + ... + x/y^n
               *
               * Using geometric series,
               *
               * d = (1 - 1/y^(n+1))/(1 - 1/y)*x
               *
               * Let a = (1 - 1/y^(n+1))/(1 - 1/y),
               *
               * d = a * x
               *
               * Find d and find its nearest page boundary, then solve for x
               *
               * x = d / a
               */

              /* Get adjustments, work out y^n */
              mx_scrollable_get_adjustments (MX_SCROLLABLE (child),
                                             &hadjust, &vadjust);
              ax = (1.0 - 1.0 / pow (y, n + 1)) / (1.0 - 1.0 / y);
              ay = (1.0 - 1.0 / pow (y, n + 1)) / (1.0 - 1.0 / y);

              /* Solving for dx */
              if (hadjust)
                {
                  mx_adjustment_get_values (hadjust, &value, &lower, &upper,
                                            &step_increment, NULL, &page_size);

                  /* Make sure we pick the next nearest step increment in the
                   * same direction as the push.
                   */
                  priv->dx *= n;
                  if (ABS (priv->dx) < step_increment / 2)
                    d = round ((value + priv->dx - lower) / step_increment);
                  else if (priv->dx > 0)
                    d = ceil ((value + priv->dx - lower) / step_increment);
                  else
                    d = floor ((value + priv->dx - lower) / step_increment);

                  if (priv->overshoot <= 0.0)
                    d = CLAMP ((d * step_increment) + lower,
                               lower, upper - page_size) - value;
                  else
                    d = ((d * step_increment) + lower) - value;

                  priv->dx = d / ax;
                }

              /* Solving for dy */
              if (vadjust)
                {
                  mx_adjustment_get_values (vadjust, &value, &lower, &upper,
                                            &step_increment, NULL, &page_size);

                  priv->dy *= n;
                  if (ABS (priv->dy) < step_increment / 2)
                    d = round ((value + priv->dy - lower) / step_increment);
                  else if (priv->dy > 0)
                    d = ceil ((value + priv->dy - lower) / step_increment);
                  else
                    d = floor ((value + priv->dy - lower) / step_increment);

                  if (priv->overshoot <= 0.0)
                    d = CLAMP ((d * step_increment) + lower,
                               lower, upper - page_size) - value;
                  else
                    d = ((d * step_increment) + lower) - value;

                  priv->dy = d / ay;
                }

              priv->deceleration_timeline = clutter_timeline_new (duration);

              g_signal_connect (priv->deceleration_timeline, "new_frame",
                                G_CALLBACK (deceleration_new_frame_cb), scroll);
              g_signal_connect (priv->deceleration_timeline, "completed",
                                G_CALLBACK (deceleration_completed_cb), scroll);
              priv->accumulated_delta = 0;
              priv->hmoving = priv->vmoving = TRUE;
              clutter_timeline_start (priv->deceleration_timeline);
              decelerating = TRUE;
            }
        }
    }

  /* Reset motion event buffer */
  priv->last_motion = 0;

  if (!decelerating)
    clamp_adjustments (scroll, 250, TRUE, TRUE);

  return TRUE;
}

static gboolean
button_press_event_cb (ClutterActor        *actor,
                       ClutterEvent        *event,
                       MxKineticScrollView *scroll)
{
  MxKineticScrollViewPrivate *priv = scroll->priv;
  ClutterButtonEvent *bevent = (ClutterButtonEvent *)event;
  ClutterActor *stage = clutter_actor_get_stage (actor);

  if ((event->type == CLUTTER_BUTTON_PRESS) &&
      (bevent->button == priv->button) &&
      stage)
    {
      MxKineticScrollViewMotion *motion;

      /* Reset motion buffer */
      priv->last_motion = 0;
      motion = &g_array_index (priv->motion_buffer, MxKineticScrollViewMotion, 0);

      if (clutter_actor_transform_stage_point (actor, bevent->x, bevent->y,
                                               &motion->x, &motion->y))
        {
          guint threshold;
          MxSettings *settings = mx_settings_get_default ();

          g_get_current_time (&motion->time);

          if (priv->deceleration_timeline)
            {
              clutter_timeline_stop (priv->deceleration_timeline);
              g_object_unref (priv->deceleration_timeline);
              priv->deceleration_timeline = NULL;
            }

          g_signal_connect (stage,
                            "captured-event",
                            G_CALLBACK (motion_event_cb),
                            scroll);
          g_signal_connect (stage,
                            "captured-event",
                            G_CALLBACK (button_release_event_cb),
                            scroll);

          /* If there's a zero drag threshold, start the drag immediately */
          g_object_get (G_OBJECT (settings),
                        "drag-threshold", &threshold, NULL);
          if (threshold == 0)
            {
              priv->in_drag = TRUE;
              clutter_set_motion_events_enabled (FALSE);

              /* Swallow the press event */
              return TRUE;
            }
          else
            priv->in_drag = FALSE;
        }
    }

  return FALSE;
}

static void
mx_kinetic_scroll_view_actor_added_cb (ClutterContainer *container,
                                       ClutterActor     *actor)
{
  MxKineticScrollViewPrivate *priv = MX_KINETIC_SCROLL_VIEW (container)->priv;

  if (MX_IS_SCROLLABLE (actor))
    {
      MxAdjustment *hadjust, *vadjust;

      priv->child = actor;

      /* Make sure the adjustments have been created so the child
       * will initialise them during its allocation (necessary for
       * MxBoxLayout, for example)
       */
      mx_scrollable_get_adjustments (MX_SCROLLABLE (actor), &hadjust, &vadjust);
    }
  else
    g_warning ("Attempting to add an actor of type %s to "
               "a MxKineticScrollView, but the actor does "
               "not implement MxScrollable.",
               g_type_name (G_OBJECT_TYPE (actor)));
}

static void
mx_kinetic_scroll_view_actor_removed_cb (ClutterContainer *container,
                                         ClutterActor     *actor)
{
  MxKineticScrollViewPrivate *priv = MX_KINETIC_SCROLL_VIEW (container)->priv;
  priv->child = NULL;
}

static void
mx_kinetic_scroll_view_init (MxKineticScrollView *self)
{
  MxKineticScrollViewPrivate *priv = self->priv =
    KINETIC_SCROLL_VIEW_PRIVATE (self);

  priv->motion_buffer =
    g_array_sized_new (FALSE, TRUE, sizeof (MxKineticScrollViewMotion), 3);
  g_array_set_size (priv->motion_buffer, 3);
  priv->decel_rate = 1.1f;
  priv->button = 1;

  clutter_actor_set_reactive (CLUTTER_ACTOR (self), TRUE);
  g_signal_connect (self, "button-press-event",
                    G_CALLBACK (button_press_event_cb), self);
  g_signal_connect (self, "actor-added",
                    G_CALLBACK (mx_kinetic_scroll_view_actor_added_cb), self);
  g_signal_connect (self, "actor-removed",
                    G_CALLBACK (mx_kinetic_scroll_view_actor_removed_cb), self);

  mx_bin_set_alignment (MX_BIN (self), MX_ALIGN_START, MX_ALIGN_START);
}

/**
 * mx_kinetic_scroll_view_new:
 *
 * Creates a new #MxKineticScrollView.
 *
 * Returns: a newly allocated #MxKineticScrollView
 */
ClutterActor *
mx_kinetic_scroll_view_new ()
{
  return g_object_new (MX_TYPE_KINETIC_SCROLL_VIEW, NULL);
}

/**
 * mx_kinetic_scroll_view_stop:
 * @scroll: A #MxKineticScrollView
 *
 * Stops any current movement due to kinetic scrolling.
 */
void
mx_kinetic_scroll_view_stop (MxKineticScrollView *scroll)
{
  MxKineticScrollViewPrivate *priv;

  g_return_if_fail (MX_IS_KINETIC_SCROLL_VIEW (scroll));

  priv = scroll->priv;

  if (priv->deceleration_timeline)
    {
      clutter_timeline_stop (priv->deceleration_timeline);
      g_object_unref (priv->deceleration_timeline);
      priv->deceleration_timeline = NULL;
    }
}

/**
 * mx_kinetic_scroll_view_set_deceleration:
 * @scroll: A #MxKineticScrollView
 * @rate: The deceleration rate
 *
 * Sets the deceleration rate when a drag is finished on the kinetic
 * scroll-view. This is the value that the momentum is divided by
 * every 60th of a second.
 */
void
mx_kinetic_scroll_view_set_deceleration (MxKineticScrollView *scroll,
                                         gdouble              rate)
{
  MxKineticScrollViewPrivate *priv;

  g_return_if_fail (MX_IS_KINETIC_SCROLL_VIEW (scroll));
  g_return_if_fail (rate >= 1.1);

  priv = scroll->priv;

  if (priv->decel_rate != rate)
    {
      priv->decel_rate = rate;
      g_object_notify (G_OBJECT (scroll), "deceleration");
    }
}

/**
 * mx_kinetic_scroll_view_get_deceleration:
 * @scroll: A #MxKineticScrollView
 *
 * Retrieves the deceleration rate of the kinetic scroll-view.
 *
 * Returns: The deceleration rate of the kinetic scroll-view
 */
gdouble
mx_kinetic_scroll_view_get_deceleration (MxKineticScrollView *scroll)
{
  g_return_val_if_fail (MX_IS_KINETIC_SCROLL_VIEW (scroll), 0.0);
  return scroll->priv->decel_rate;
}

/*
void
mx_kinetic_scroll_view_set_buffer_size (MxKineticScrollView *scroll,
                                        guint                size)
{
  MxKineticScrollViewPrivate *priv;

  g_return_if_fail (MX_IS_KINETIC_SCROLL_VIEW (scroll));
  g_return_if_fail (size > 0);

  priv = scroll->priv;
  if (priv->motion_buffer->len != size)
    {
      g_array_set_size (priv->motion_buffer, size);
      g_object_notify (G_OBJECT (scroll), "buffer-size");
    }
}

guint
mx_kinetic_scroll_view_get_buffer_size (MxKineticScrollView *scroll)
{
  g_return_val_if_fail (MX_IS_KINETIC_SCROLL_VIEW (scroll), 0);
  return scroll->priv->motion_buffer->len;
}
*/

/**
 * mx_kinetic_scroll_view_set_mouse_button:
 * @scroll: A #MxKineticScrollView
 * @button: A mouse button number
 *
 * Sets the mouse button number used to initiate drag events on the kinetic
 * scroll-view.
 */
void
mx_kinetic_scroll_view_set_mouse_button (MxKineticScrollView *scroll,
                                         guint32              button)
{
  MxKineticScrollViewPrivate *priv;

  g_return_if_fail (MX_IS_KINETIC_SCROLL_VIEW (scroll));

  priv = scroll->priv;

  if (priv->button != button)
    {
      priv->button = button;
      g_object_notify (G_OBJECT (scroll), "mouse-button");
    }
}

/**
 * mx_kinetic_scroll_view_get_mouse_button:
 * @scroll: A #MxKineticScrollView
 *
 * Gets the #MxKineticScrollView:mouse-button property
 *
 * Returns: The mouse button number used to initiate drag events on the
 *          kinetic scroll-view
 */
guint32
mx_kinetic_scroll_view_get_mouse_button (MxKineticScrollView *scroll)
{
  g_return_val_if_fail (MX_IS_KINETIC_SCROLL_VIEW (scroll), 0);
  return scroll->priv->button;
}

/**
 * mx_kinetic_scroll_view_set_use_captured:
 * @scroll: A #MxKineticScrollView
 * @use_captured: %TRUE to use captured events
 *
 * Sets whether to use captured events to initiate drag events. This can be
 * used to block events that would initiate scrolling from reaching the child
 * actor.
 */
void
mx_kinetic_scroll_view_set_use_captured (MxKineticScrollView *scroll,
                                         gboolean             use_captured)
{
  MxKineticScrollViewPrivate *priv;

  g_return_if_fail (MX_IS_KINETIC_SCROLL_VIEW (scroll));

  priv = scroll->priv;
  if (priv->use_captured != use_captured)
    {
      priv->use_captured = use_captured;

      g_signal_handlers_disconnect_by_func (scroll,
                                            button_press_event_cb,
                                            scroll);

      g_signal_connect (scroll,
                        use_captured ? "captured-event" : "button-press-event",
                        G_CALLBACK (button_press_event_cb),
                        scroll);

      g_object_notify (G_OBJECT (scroll), "use-captured");
    }
}

/**
 * mx_kinetic_scroll_view_get_use_captured:
 * @scroll: A #MxKineticScrollView
 *
 * Gets the #MxKineticScrollView:use-captured property.
 *
 * Returns: %TRUE if captured-events should be used to initiate scrolling
 */
gboolean
mx_kinetic_scroll_view_get_use_captured (MxKineticScrollView *scroll)
{
  g_return_val_if_fail (MX_IS_KINETIC_SCROLL_VIEW (scroll), FALSE);
  return scroll->priv->use_captured;
}

/**
 * mx_kinetic_scroll_view_set_overshoot:
 * @scroll: A #MxKineticScrollView
 * @overshoot: The rate at which the view will decelerate when scrolling beyond
 *             its boundaries.
 *
 * Sets the rate at which the view will decelerate when scrolling beyond its
 * boundaries. The deceleration rate will be multiplied by this value every
 * 60th of a second when the view is scrolling outside of the range set by its
 * adjustments.
 *
 * See mx_kinetic_scroll_view_set_deceleration()
 */
void
mx_kinetic_scroll_view_set_overshoot (MxKineticScrollView *scroll,
                                      gdouble              overshoot)
{
  MxKineticScrollViewPrivate *priv;

  g_return_if_fail (MX_IS_KINETIC_SCROLL_VIEW (scroll));

  priv = scroll->priv;
  if (priv->overshoot != overshoot)
    {
      priv->overshoot = overshoot;
      g_object_notify (G_OBJECT (scroll), "overshoot");
    }
}

/**
 * mx_kinetic_scroll_view_get_overshoot:
 * @scroll: A #MxKineticScrollView
 *
 * Retrieves the deceleration rate multiplier used when the scroll-view is
 * scrolling beyond its boundaries.
 */
gdouble
mx_kinetic_scroll_view_get_overshoot (MxKineticScrollView *scroll)
{
  g_return_val_if_fail (MX_IS_KINETIC_SCROLL_VIEW (scroll), 0.0);
  return scroll->priv->overshoot;
}