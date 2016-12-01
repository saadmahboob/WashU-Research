#include "widget.h"
#include "ui_widget.h"

#include <iomanip>
#include <iostream>
#include <sstream>

#include <GL/gl.h>
#include <GL/glu.h>

#include <pcl/filters/statistical_outlier_removal.h>
#include <pcl/filters/uniform_sampling.h>
#include <pcl/io/ply_io.h>

DEFINE_double(omega, 0.002, "Spin rate in radians per second");
DEFINE_double(h_velocity, 0, "Velocity of the y clipping plane in m/s");
DEFINE_double(d_velocity, 0, "Velocity of the distance from the center in m/s");
DEFINE_double(h_min, 0, "Min height of the h_clipping_plane");
DEFINE_double(d_min, 10, "Min distance");
DEFINE_double(FPS, 1, "target fps");

template <typename ArrayTypeA, typename ArrayTypeB>
double gaussianWeight(const ArrayTypeA &pos, const ArrayTypeB &s) {
  return std::exp(-(pos.square() / (2 * s.square())).sum());
}

constexpr double h_bin_size = 0.01, h_bin_scale = 1. / h_bin_size;

constexpr double xRot = 0, yRot = 0, zRot = 0;

constexpr double fov = 60;

static const Eigen::Vector3d look_vector_start =
    Eigen::Vector3d(0, 5, 8).normalized();

struct VertexData {
  float data[6];
};

int Widget::binner(float y) {
  return std::max(
      0, std::min(static_cast<int>(std::round(h_bin_scale * (y - min[1]))),
                  static_cast<int>(h_bins.size() - 1)));
}

Widget::Widget(const std::string &name, const std::string &out_folder,
               QWidget *parent)
    : QOpenGLWidget(parent), ui(new Ui::Widget),
      cloud{new pcl::PointCloud<PointType>}, k_up{0, 1, 0}, frame_counter{0},
      render{false}, radians_traveled{0}, omega{FLAGS_omega / FLAGS_FPS},
      current_state{pure_rotation}, start_PI{0}, h_v{0}, d_v{0}, e_v{0},
      recorder{out_folder}, dist_to_spin{PI / 2.},
      state_after_spin{plane_down} {
  ui->setupUi(this);

  pcl::io::loadPLYFile(name, *cloud);

  // filter();
  bounding_box();

  // timer.start(1000 / FPS, this);

  render = true;
}

Widget::~Widget() { delete ui; }

void Widget::allocate() {
  std::cout << "allocating" << std::endl;

  std::sort(cloud->begin(), cloud->end(),
            [](PointType &a, PointType &b) { return a.z < b.z; });

  h_bins.resize(h_bin_scale * (cloud->at(cloud->size() - 1).z - min[1]) + 1, 0);
  h_clipping_plane = cloud->at(cloud->size() - 1).z + 1.0;
  std::vector<VertexData> points;
  std::vector<int> idx_data;
  int idx_counter = 0;
  int bin_index = binner(cloud->at(0).z);
  for (auto &p : *cloud) {
    VertexData tmp;
    tmp.data[0] = p.x;
    tmp.data[1] = p.z;
    tmp.data[2] = p.y;

    tmp.data[3] = p.r / 255.;
    tmp.data[4] = p.g / 255.;
    tmp.data[5] = p.b / 255.;

    points.emplace_back(tmp);

    int new_bin_index = binner(p.z);
    if (new_bin_index != bin_index) {
      for (int i = bin_index; i < new_bin_index; ++i)
        h_bins[i] = points.size() - 1;

      bin_index = new_bin_index;
    }

    idx_data.emplace_back(idx_counter++);
  }

  for (int i = bin_index; i < h_bins.size(); ++i)
    h_bins[i] = points.size();

  std::cout << points.size() << std::endl;
  std::cout << h_bins[h_bins.size() - 1] << std::endl;

  size_t bytes_allocated = 0;
  size_t points_buffered = 0;
  size_t max_p = 0;
  for (int i = 0; true; ++i) {
    const long bytes =
        std::min(static_cast<size_t>(std::numeric_limits<int>::max()) / 2,
                 sizeof(VertexData) * points.size() - bytes_allocated);

    if (bytes <= 0)
      break;

    const size_t p = bytes / sizeof(VertexData);

    vertex_buffers.push_back(std::unique_ptr<QOpenGLBuffer>(
        new QOpenGLBuffer(QOpenGLBuffer::VertexBuffer)));
    vertex_buffers[i]->create();
    vertex_buffers[i]->bind();
    vertex_buffers[i]->setUsagePattern(QOpenGLBuffer::StaticDraw);
    vertex_buffers[i]->allocate(points.data() + points_buffered, bytes);
    vertex_buffers[i]->release();

    bytes_allocated += bytes;
    points_buffered += p;

    max_p = std::max(max_p, p);

    buffer_sizes.push_back(p);
  }

  index_buffer = std::unique_ptr<QOpenGLBuffer>(
      new QOpenGLBuffer(QOpenGLBuffer::IndexBuffer));
  index_buffer->create();
  index_buffer->setUsagePattern(QOpenGLBuffer::DynamicDraw);
  index_buffer->bind();
  index_buffer->allocate(max_p * sizeof(int));
  index_buffer->release();

  /* clang-format off */
  //                          x     y
  static float quadVertices[] = {
      // Positions   // TexCoords
      -1.0f,  1.0f,  /*0.0f, 1.0f,*/
      -1.0f, -1.0f,  /*0.0f, 0.0f,*/
       1.0f, -1.0f,  /*1.0f, 0.0f,*/

      -1.0f,  1.0f,  /*0.0f, 1.0f,*/
       1.0f, -1.0f,  /*1.0f, 0.0f,*/
       1.0f,  1.0f,  /*1.0f, 1.0f*/
  };
  /* clang-format on */

  aa_buffer = std::unique_ptr<QOpenGLBuffer>(
      new QOpenGLBuffer(QOpenGLBuffer::VertexBuffer));
  aa_buffer->create();
  aa_buffer->bind();
  aa_buffer->setUsagePattern(QOpenGLBuffer::StaticDraw);
  aa_buffer->allocate(quadVertices, sizeof(quadVertices));
  aa_buffer->release();
}

void Widget::initializeGL() {
  initializeOpenGLFunctions();
  this->makeCurrent();
  glClearColor(0, 0, 0, 0);

  glEnable(GL_BLEND);
  // glBlendFunc(GL_ONE, GL_ONE);
  glEnable(GL_DEPTH_TEST);
  glEnable(GL_CULL_FACE);
  glShadeModel(GL_SMOOTH);
  glEnable(GL_MULTISAMPLE);
  // glEnable(GL_POINT_SMOOTH);
  // glEnable(GL_LIGHTING);
  glEnable(GL_ARB_texture_non_power_of_two);
  glEnable(GL_ARB_arrays_of_arrays);

  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glEnable(GL_TEXTURE_2D);

  glEnable(GL_PROGRAM_POINT_SIZE);
  constexpr double point_size_init = 1.5;
  glPointSize(point_size_init);

  GLfloat attenuations_params[] = {1, 2, 2};

  glPointParameterf(GL_POINT_SIZE_MIN, 0.2);
  // glPointParameterf(GL_POINT_SIZE_MAX, 10.0);
  glPointParameterf(GL_POINT_FADE_THRESHOLD_SIZE, 0.25);
  glPointParameterfv(GL_POINT_DISTANCE_ATTENUATION, attenuations_params);

  aa_program = std::unique_ptr<QOpenGLShaderProgram>(
      new QOpenGLShaderProgram(context()));

  aa_program->addShaderFromSourceCode(
      QOpenGLShader::Vertex,
      "attribute vec2 position, tex_coord;\n"
      "varying vec2 v_tex_coord;\n"

      "void main() {\n"
      "  v_tex_coord = position * 0.5 + 0.5;\n"
      "  gl_Position = vec4(position.x, position.y, 0.0, 1.0);\n"
      "}");
  aa_program->addShaderFromSourceCode(
      QOpenGLShader::Fragment, "uniform sampler2D texture;\n"
                               "varying vec2 v_tex_coord;\n"
                               "uniform vec2 viewport;\n"
                               "uniform float weights [25];\n"
                               "void main() {\n"
                               "  vec3 color = vec3(0.0);\n"
                               "  for (int j = 0; j < 5; j++) {\n"
                               "    for (int i = 0; i < 5; i++) {\n"
                               "      vec2 off = vec2(i - 2, j - 2)/viewport;\n"
                               "      color += texture2D(texture, "
                               "v_tex_coord + off).rgb * weights[j*5 + i];\n"
                               "    }\n"
                               "  }\n"
                               "  gl_FragColor = vec4(color, 1.0);\n"
                               "}");

  aa_program->link();
  aa_program->bind();
  position_location = aa_program->attributeLocation("position");
  sampler_location = aa_program->uniformLocation("texture");
  texcoord_location = aa_program->attributeLocation("tex_coord");
  viewport_location = aa_program->uniformLocation("viewport");
  float values[25];

  static const Eigen::Array2d sigma(0.5, 0.5);
  double sum = 0;
  for (int j = 0; j < 5; ++j) {
    for (int i = 0; i < 5; ++i) {
      double v = gaussianWeight(Eigen::Array2d(i - 2, j - 2), sigma);
      values[j * 5 + i] = v;
      sum += v;
    }
  }
  for (int i = 0; i < 25; ++i)
    values[i] /= sum;

  aa_program->setUniformValueArray("weights", values, 25, 1);
  aa_program->release();

  cloud_program = std::unique_ptr<QOpenGLShaderProgram>(
      new QOpenGLShaderProgram(context()));

  cloud_program->addShaderFromSourceCode(
      QOpenGLShader::Vertex, "uniform mat4 mvp_matrix;\n"
                             "attribute vec3 vertex;\n"
                             "attribute vec3 color;\n"
                             "varying vec3 v_color;\n"
                             "void main() {\n"
                             "  v_color = color;\n"
                             "  gl_Position = mvp_matrix * vec4(vertex, 1.0);\n"
                             "}");
  cloud_program->addShaderFromSourceCode(
      QOpenGLShader::Fragment, "varying vec3 v_color;\n"
                               "void main() {\n"
                               "  gl_FragColor = vec4(v_color, 1.0);\n"
                               "}");
  cloud_program->link();
  cloud_program->bind();

  vertex_location = cloud_program->attributeLocation("vertex");
  color_location = cloud_program->attributeLocation("color");

  allocate();
}

void Widget::set_next_state() {
  switch (current_state) {
  case pure_rotation:
    if (radians_traveled - start_PI >= dist_to_spin)
      current_state = state_after_spin;
    break;

  case plane_down:
    if (h_clipping_plane <= FLAGS_h_min && distance <= FLAGS_d_min)
      current_state = zoom_in;
    break;

  case zoom_in:
    if (distance <= FLAGS_d_min) {
      current_state = pure_rotation;
      start_PI = radians_traveled;
      state_after_spin = zoom_out;
    }
    break;

  case zoom_out:
    if (distance >= start_distance) {
      current_state = pure_rotation;
      start_PI = radians_traveled;
      dist_to_spin = PI / 3.0;
      state_after_spin = plane_final;
    }
    break;

  case plane_up:
    if (h_clipping_plane >= max[1])
      current_state = pure_rotation;

    break;

  case plane_final:
    if (h_bins[binner(h_clipping_plane)] == 0)
      current_state = done;
    break;

  default:
    break;
  }
}

void Widget::do_state_outputs() {
  switch (current_state) {
  case pure_rotation:
    e_v = (e_v + omega) / 2.0;
    for (auto &&c : cubes)
      c.rotate(-e_v);

    break;

  case plane_down:
    if (h_clipping_plane >= FLAGS_h_min) {
      h_v = std::max(
          0.5 * std::min(FLAGS_h_velocity, (h_clipping_plane - FLAGS_h_min)) +
              0.5 * h_v,
          0.1 * FLAGS_h_velocity);
      h_clipping_plane -= h_v / FLAGS_FPS;
    }

    if (h_bins[binner(h_clipping_plane)] < cloud->size() &&
        distance >= FLAGS_d_min) {
      d_v =
          std::max(0.5 * std::min(FLAGS_d_velocity, (distance - FLAGS_d_min)) +
                       0.5 * d_v,
                   0.1 * FLAGS_d_velocity);
      distance -= d_v / FLAGS_FPS;
      eye_y -= d_v / FLAGS_FPS * std::abs(look_vector_start[1]);
      camera_y += d_v / FLAGS_FPS * std::abs(look_vector_start[1]) / 2.0;
      camera_y = std::min(2.5, camera_y);
    }

    break;

  case zoom_in:
    d_v = std::max(0.5 * std::min(FLAGS_d_velocity, (distance - FLAGS_d_min)) +
                       0.5 * d_v,
                   0.1 * FLAGS_d_velocity);
    distance -= d_v / FLAGS_FPS;
    eye_y -= d_v / FLAGS_FPS * std::abs(look_vector_start[1]);
    break;

  case zoom_out:
    d_v =
        std::max(0.5 * std::min(FLAGS_d_velocity, (start_distance - distance)) +
                     0.5 * d_v,
                 0.1 * FLAGS_d_velocity);
    distance += 2 * d_v / FLAGS_FPS;
    eye_y += 2 * d_v / FLAGS_FPS * std::abs(look_vector_start[1]);
    break;

  case plane_up:
    h_v =
        std::max(0.5 * std::min(FLAGS_h_velocity, (max[1] - h_clipping_plane)) +
                     0.5 * h_v,
                 0.1 * FLAGS_h_velocity);
    h_clipping_plane += 2 * h_v / FLAGS_FPS;
    break;

  case done:
    if (render == true)
      recorder.exit();
    render = false;
    std::cout << "DONE" << std::endl;
    QApplication::quit();
    break;

  case plane_final:
    h_v = std::max(
        0.5 * std::min(FLAGS_h_velocity, (h_clipping_plane - FLAGS_h_min)) +
            0.5 * h_v,
        0.1 * FLAGS_h_velocity);
    h_clipping_plane -= 5.0 * h_v / FLAGS_FPS;
    e_v *= 0.999;
    break;

  default:
    break;
  }
}

void Widget::set_matrices() {

  Eigen::Vector2d look_vector = rails_eye - camera_origin;
  look_vector.normalize();

  eye = camera_origin + distance * look_vector;

  if (render) {
    Eigen::Vector2d perp_vector(-look_vector[1], look_vector[0]);
    perp_vector.normalize();

    const double eye_distance = (eye - camera_origin).norm();
    eye += e_v * eye_distance * perp_vector;

    const double rails_distance = (rails_eye - camera_origin).norm();
    rails_eye += e_v * rails_distance * perp_vector;

    radians_traveled += e_v;
  }
  QMatrix4x4 matrix;
  matrix.lookAt(QVector3D(eye[0], eye_y, eye[1]),
                QVector3D(camera_origin[0], camera_y, camera_origin[1]),
                QVector3D(0, 1, 0));

  mvp = projection * matrix;
}

void Widget::capture() {
  if (!render || !FLAGS_save)
    return;

  uchar *buf = buffer;
  glReadPixels(0, 0, width(), height(), GL_RGB, GL_UNSIGNED_BYTE, buf);

  for (int j = 0; j < height(); ++j) {
    uchar *dst = img.ptr<uchar>(height() - 1 - j);
    for (int i = 0; i < width(); ++i) {
      dst[3 * i + 0] = buf[3 * (j * width() + i) + 2];
      dst[3 * i + 1] = buf[3 * (j * width() + i) + 1];
      dst[3 * i + 2] = buf[3 * (j * width() + i) + 0];
    }
  }

  recorder.submit_frame(img.clone());
}

void Widget::paintGL() {
  set_matrices();
  draw();
  capture();

  set_next_state();
  do_state_outputs();

  if (render)
    update();
}

void Widget::resizeGL(int width, int height) {

  buffer.allocate(width * height * 3);
  img = cv::Mat(height, width, CV_8UC3);

  aa_width = width * aa_factor;
  aa_height = height * aa_factor;

  projection.setToIdentity();
  // Set perspective projection
  projection.perspective(fov, width / static_cast<double>(height), 1 / 10.,
                         200.);
}

bool Widget::is_in_cube(PointType &p) {
  for (auto &cube : cubes)
    if (cube.is_in(p))
      return true;

  return false;
}

void Widget::draw() {
  glEnable(GL_DEPTH_TEST);
  glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
  glViewport(0, 0, aa_width, aa_height);
  cloud_program->bind();

#if 1
  QOpenGLFramebufferObjectFormat format;
  format.setAttachment(QOpenGLFramebufferObject::NoAttachment);
  format.setSamples(4);
  render_fbo = std::unique_ptr<QOpenGLFramebufferObject>(
      new QOpenGLFramebufferObject(aa_width, aa_height, format));

  if (!render_fbo->isValid())
    std::cout << "buffer invalid!" << std::endl;

  if (!render_fbo->bind())
    std::cout << "Could not bind buffer!" << std::endl;
  glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
#endif
  cloud_program->setUniformValue("mvp_matrix", mvp);

  int idx = 0;
  size_t points_drawn = 0;
  for (auto &&v : vertex_buffers) {
    long num_points_to_draw = std::min(
        buffer_sizes[idx], h_bins[binner(h_clipping_plane)] - points_drawn);
    if (num_points_to_draw <= 0)
      break;
    v->bind();

    quintptr offset = 0;

    cloud_program->enableAttributeArray(vertex_location);
    cloud_program->setAttributeBuffer(vertex_location, GL_FLOAT, offset, 3,
                                      sizeof(VertexData));
    offset += 3 * sizeof(float);

    cloud_program->enableAttributeArray(color_location);
    cloud_program->setAttributeBuffer(color_location, GL_FLOAT, offset, 3,
                                      sizeof(VertexData));

    std::vector<uint32_t> indicies;
    for (uint32_t i = 0; i < num_points_to_draw; ++i)
      if (!is_in_cube(cloud->at(points_drawn + i)))
        indicies.emplace_back(i);

    if (indicies.size() > 0) {

      index_buffer->bind();
      index_buffer->write(0, indicies.data(), indicies.size() * sizeof(int));

      glDrawElements(GL_POINTS, indicies.size(), GL_UNSIGNED_INT, 0);
      index_buffer->release();
    }

    points_drawn += num_points_to_draw;
    v->release();
    ++idx;
  }

  cloud_program->release();

#if 1

  texture_fbo = std::unique_ptr<QOpenGLFramebufferObject>(
      new QOpenGLFramebufferObject(aa_width, aa_height));
  QRect rect(0, 0, render_fbo->width(), render_fbo->height());
  QOpenGLFramebufferObject::blitFramebuffer(texture_fbo.get(), rect,
                                            render_fbo.get(), rect);

  if (!QOpenGLFramebufferObject::bindDefault())
    std::cout << "Could not bind default!" << std::endl;

  glViewport(0, 0, aa_width / aa_factor, aa_height / aa_factor);
  aa_program->bind();
  glDisable(GL_DEPTH_TEST);

  aa_buffer->bind();
  quintptr offset = 0;
  aa_program->enableAttributeArray(position_location);
  aa_program->setAttributeBuffer(position_location, GL_FLOAT, offset, 2, 0);

  /*offset += 2 * sizeof(float);

  aa_program->enableAttributeArray(texcoord_location);
  aa_program->setAttributeBuffer(texcoord_location, GL_FLOAT, offset, 2,
                                 4 * sizeof(float));*/

  glBindTexture(GL_TEXTURE_2D, texture_fbo->texture());
  aa_program->setUniformValue(sampler_location, 0);
  aa_program->setUniformValue(
      viewport_location,
      QVector2D(aa_height / aa_factor, aa_width / aa_factor));

  glDrawArrays(GL_TRIANGLES, 0, 6);
  aa_program->release();
#endif
}

void Widget::bounding_box() {
  Eigen::Array3d average = Eigen::Array3d::Zero();
  for (auto &point : *cloud)
    average += point.getVector3fMap().cast<double>().array();

  average /= cloud->size();

  Eigen::Array3d sigma = Eigen::Array3d::Zero();

  for (auto &point : *cloud) {
    auto tmp = point.getVector3fMap().cast<double>().array();
    sigma += (tmp - average) * (tmp - average);
  }

  sigma /= cloud->size() - 1;
  sigma.sqrt();

  static const Eigen::Array3d delta(5.5, 5.5, 5.5);

  max = average + delta * sigma / 2.0;
  min = average - delta * sigma / 2.0;

  std::cout << max << std::endl << std::endl << min << std::endl << std::endl;

  cloud->erase(std::remove_if(cloud->begin(), cloud->end(),
                              [&](auto &p) {
                                bool in = true;
                                for (int i = 0; i < 3; ++i)
                                  if (p.getVector3fMap()[i] < min[i] ||
                                      p.getVector3fMap()[i] > max[i])
                                    in = false;

                                return !in;
                              }),
               cloud->end());

  double tmp = max[1];
  max[1] = max[2];
  max[2] = tmp;

  tmp = min[1];
  min[1] = min[2];
  min[2] = tmp;

  floor_box =
      Eigen::Vector2d(std::abs(max[0] - min[0]), std::abs(max[2] - min[2]));

  distance = std::max(
      10.0, std::min(60.0, std::sqrt((sigma * delta).square().sum()) / 2.0));
  start_distance = distance;

  camera_origin = Eigen::Vector2d(average[0], average[1]);
  camera_y = 0;

  Eigen::Vector3d start_eye =
      Eigen::Vector3d(camera_origin[0], 0, camera_origin[1]) +
      distance * look_vector_start;

  eye = Eigen::Vector2d(start_eye[0], start_eye[2]);
  eye_y = start_eye[1];
  rails_eye = eye;

  org_camera = camera_origin;
  org_eye = eye;

  h_clipping_plane = max[1];

  static const Eigen::Vector3d diag = Eigen::Vector3d(1, 1, 1).normalized();
  cubes.emplace_back(Eigen::Vector3d(average[0], average[2], average[1]),
                     Eigen::Vector3d(average[0], average[2], average[1]) +
                         15 * diag);
  cubes.back().growX(20);
  cubes.back().growZ(20);
  cubes.back().growY(3);
}

void Widget::timerEvent(QTimerEvent *) { update(); }

void Widget::filter() {
  pcl::UniformSampling<PointType> uniform_sampling;
  uniform_sampling.setInputCloud(cloud);
  cloud = pcl::PointCloud<PointType>::Ptr(new pcl::PointCloud<PointType>);
  uniform_sampling.setRadiusSearch(1e-3);
  uniform_sampling.filter(*cloud);

  /* pcl::StatisticalOutlierRemoval<PointType> sor;
   sor.setInputCloud(cloud);
   sor.setMeanK(50);
   sor.setStddevMulThresh(2.0);
   cloud = pcl::PointCloud<PointType>::Ptr(new pcl::PointCloud<PointType>);
   sor.filter(*cloud);*/

  std::cout << cloud->size() << std::endl;
}

void Widget::keyPressEvent(QKeyEvent *e) {
  if (e->key() == Qt::Key_S) {
    render = !render;
    if (render)
      update();
  } else if (e->key() == Qt::Key_R) {
    render = false;
    frame_counter = 0;

    camera_origin = org_camera;
    eye = org_eye;
    rails_eye = org_eye;

    radians_traveled = 0;
    h_clipping_plane = max[1];

    distance = start_distance;

    update();
  } else if (e->key() == Qt::Key_J) {
    distance += 1.0;
    update();
  } else if (e->key() == Qt::Key_K) {
    distance -= 1.0;
    update();
  }
}